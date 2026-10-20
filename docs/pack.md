# The pack: format, reader and tool

Status: built, 2026-10-08. Phase 1 settled the decisions that rig_spec 5
left open (the coordinator approved them, with the recommendations of
section 10 as defaults); phase 2 built them: `include/ysp/pack.h` v0.1.0
(the reader), `pack/ysp/pack_tool.h` v0.1.0 (the tool library) and the
`ypak` CLI. Section 9.1 has the phase 1 probes and the phase 2
measurements. Phase 3 (2026-10-13, tool v0.4.0, section 13) added video
with its index, frame sequences, tables from `.xlsx` workbooks and
textures from JPEG files; the reader did not change. Tool v0.5.0
(2026-10-20, section 14) runs the ffmpeg step on a recorded thread count
instead of one thread. Where the build
changed the design, the text says so. When this page and rig_spec 5
disagree, this page is newer.

Settled before this page, in rig_spec 5, and not reopened here: one zip
file in store-only mode; entries aligned to 4 KB; readable by any zip
tool; a browser reads the central directory from the tail and then only
the entries it needs, by HTTP range; compression per resource, never on
the container; the pack can be appended to the player executable; a
manifest with a format version and, per entry, a checksum, the source
file's hash, the tool version and the exact derivation; the runtime
refuses an unknown format version; canonical in-memory forms equal the
on-disk forms (audio is widened to float at load).

## 1. Summary of the decisions

| Question | Decision | Reason |
|---|---|---|
| zip64 | Yes, only where a value does not fit 32 bits (APPNOTE 4.3.9.2) | A two-hour film is 7.2 GB at 8 Mbit/s. Six zip readers read a 4.4 GB stored zip64 entry (measured, 3.3). |
| Alignment | Android's alignment extra field (0xD935) in the local header pads each entry's data to 4096 | No gaps between entries, so streaming readers work. Six readers accept it (measured). |
| Appended to an executable | The tool pads the executable to 4096 and writes absolute offsets in the copy | .NET refuses offsets relative to the zip start; every reader accepts absolute ones (measured). |
| Entry order | `ysp/manifest.json` first, then every other entry in bytewise name order | A streaming reader sees the manifest first. Sorted names give lookup by binary search and a duplicate check in one pass. |
| Names | The reader accepts safe UTF-8. The tool writes a portable ASCII set, unique without regard to case | Extraction is safe on every file system; a later tool can widen the set and old readers still open its packs. |
| Runtime checksum | XXH64 over 1 MiB chunks, then over the list of chunk hashes | 10 to 12 GB/s native and in WebAssembly; on the cold and mapped paths XXH3 is only 10 to 12 % faster; ysp/video.h already uses XXH64. |
| Citable hashes | SHA-256 of every entry and every source, in the manifest. The pack ID is the SHA-256 of the manifest | Standard tools compute SHA-256. The ID is the same for the pack alone and for its appended copy. |
| zip CRC-32 | Written, checked by `ypak verify`, not checked at run time | Zip requires it. Slice-by-8 runs at 2.5 to 3.0 GB/s natively and 1.5 to 1.8 GB/s in WebAssembly. |
| Manifest | JSON, first entry, keys sorted, integers only, reals as decimal strings, no compiler named | People, Python and the browser read it without ysp code. The reader header does not parse it. One fixed form makes the bytes deterministic. |
| What the reader needs | A 32-byte private extra field (0x7379) in each central directory record: kind, flags, data offset, XXH64, chunk index | The reader gets everything from the central directory, which a browser fetches in one or two requests. |
| Format version | In the zip comment and in the manifest; they must agree | The reader finds it without JSON. |
| Reader header | `include/ysp/pack.h`, prefix `ypak_`, in the `ysp::core` group | It needs no SDL and no GL; file mapping is the OS's. |
| Tool | Library `pack/ysp/pack_tool.h` (prefix `ypt_`), CLI `ypak` | |
| Source description | JSON. The manifest is the source description plus the results | `ypak rebuild` uses the manifest as its input. |
| Rebuild check | Check every source's SHA-256, rebuild, then compare byte for byte | The same inputs and tool version give the same bytes (section 7); MSVC, MinGW, emcc and Linux gcc builds of the tool gave the same bytes on 213 MB of corpus (9.1). |

## 2. Reference: the container

A pack is a zip file (APPNOTE 6.3.10) with the profile below. The reader
refuses a file that is outside the profile, with a message that names the
field and the tool that writes a correct pack. File extension:
`.ysppak`.

### 2.1 Layout

```
[ executable, padded with zeros to a multiple of 4096 ]   only in an appended copy
local header 0   "ysp/manifest.json"   extra: [zip64] 0xD935 padding
data 0           at a multiple of 4096 from the zip start
local header 1   next name in bytewise order
data 1
...
local header n-1
data n-1
central directory   n records, same order, each with the 0x7379 record
[ zip64 end record, zip64 locator ]      only when needed
end of central directory record, comment "ysp-pack/1 ..."
```

- There are no gaps. Local header i + 1 starts at the end of data i. The
  central directory starts at the end of the last entry's data. The
  reader checks this, so the only bytes that no hash covers are the
  zero padding (checked) and the comment (fixed syntax).
- "The zip start" is the offset of local header 0 in the file: 0 for a
  pack alone, the padded executable size for an appended copy.

### 2.2 Fixed fields

Every field below has one value, so the container bytes follow from the
entries alone (section 7).

| Field | Value |
|---|---|
| Compression method | 0 (stored) |
| General purpose flags | 0x0800 (UTF-8 names). Bit 0 (encryption) and bit 3 (data descriptor) are refused |
| Version needed | 20; 45 when the entry has a zip64 field |
| Version made by | 0x0314 (UNIX, 2.0); 0x032D when zip64 |
| DOS time, date | 0, 0x0021 (1980-01-01 00:00) |
| CRC-32 | of the data, in both headers |
| Sizes | compressed = uncompressed; 0xFFFFFFFF in both header fields and the real sizes in the zip64 field when size >= 0xFFFFFFFF |
| Internal and external attributes | 0, and 0100644 << 16 (a regular file, rw-r--r--) |
| Disk numbers | 0 |
| File comment | none |
| Other extra fields | none (no timestamps, no UID or GID) |

### 2.3 zip64

The tool writes zip64 fields only where APPNOTE requires them:

- An entry of 0xFFFFFFFF bytes or more: a zip64 field in its local header
  with both sizes (APPNOTE 4.5.3), and in its central record with both
  sizes.
- A local header offset of 0xFFFFFFFF or more: that offset in the central
  record's zip64 field, after any sizes.
- A central directory offset of 0xFFFFFFFF or more, or 65535 entries or
  more: the zip64 end record and locator. The end record then holds
  0xFFFFFFFF or 0xFFFF in the fields that overflow.

The rule depends only on sizes and offsets, so the output is the same
every time. The reader accepts the zip64 field whenever it is present and
checks that its values agree with the 32-bit fields that do not overflow.

### 2.4 Alignment

The local header's extra field ends with Android's alignment record
(APPNOTE extra field list: 0xD935, "Android ZIP Alignment Extra Field"):

| Bytes | Value |
|---|---|
| 2 | 0xD935 |
| 2 | 2 + p |
| 2 | 4096, the alignment |
| p | zeros |

p is the smallest value that puts the data at a multiple of 4096 from the
zip start. The record is at least 6 bytes, so p can be 0 and is at most
4095. The central directory carries no padding.

Why 4096 holds in memory too: the reader maps the whole file (2.10). A
mapping starts on a page boundary, so data that is 4096-aligned in the
file is 4096-aligned in memory. Apple silicon and some Linux arm64
kernels have 16 KB or 64 KB pages; whole-file mapping makes that
irrelevant. Unbuffered I/O (512 or 4096-byte sectors) works on entry
boundaries.

### 2.5 The private central directory record (0x7379)

Each central directory record carries this extra field after any zip64
field. 0x7379 is not in APPNOTE 6.3.10's list of header IDs. Zip tools
skip it.

| Offset | Size | Field |
|---|---|---|
| 0 | 2 | 0x7379 |
| 2 | 2 | 28, the data size (the record is 32 bytes) |
| 4 | 1 | record version, 1 |
| 5 | 1 | 0 |
| 6 | 2 | kind (2.11) |
| 8 | 4 | flags: bit 0 STREAM (read through a cursor, verified per chunk); other bits 0 |
| 12 | 8 | data offset from the zip start |
| 20 | 8 | entry hash (2.8) |
| 28 | 4 | index of the entry's first hash in `ysp/chunks`, or 0xFFFFFFFF for an entry of at most one chunk |

The data offset lets a browser fetch an entry with one range request
from the central directory alone. The reader checks it against the local
header (30 + name + extra) when it first reads the entry.

### 2.6 The comment

The end record's comment is ASCII, one line, no line end:

```
ysp-pack/1 manifest=sha256:<64 hex> cd=xxh64:<16 hex>
```

- `1` is the format version. The reader refuses any other value: "pack
  format 2; this player reads format 1; update the player".
- `manifest=` is the pack ID (2.8).
- `cd=` is the XXH64 (seed 0) of the central directory bytes. A zip tool
  that changes the pack keeps the comment and changes the directory, so
  the reader then refuses the pack: "the central directory does not match
  its hash; was the pack changed by another zip tool? Rebuild it with
  ypak".

### 2.7 Names

Reader rule (what any pack may contain): valid UTF-8 (no overlong forms,
no surrogates), 1 to 1024 bytes, no byte below 0x20, no 0x7F, no `\`, no
`/` at the start or end, no empty component, no `.` or `..` component.
The tool's `extract` applies the writer rule to every name before it
writes a file.

Writer rule (what the phase 2 tool writes): components of `A`-`Z`,
`a`-`z`, `0`-`9`, `.`, `_`, `-`, joined by `/`; a component does not
start with `.` and does not end with `.`; a component is not a Windows
device name (CON, PRN, AUX, NUL, COM1 to COM9, LPT1 to LPT9, with or
without an extension); each component at most 255 bytes; no two names
equal after ASCII case folding.

Lookup is exact, byte for byte, and case-sensitive. When a lookup fails,
the reader's message names a case-insensitive match if one exists.

Reserved: names that start with `ysp/` are the format's own entries
(`ysp/manifest.json`, `ysp/chunks`). The tool refuses them in a source
description.

The extension of a name states its canonical form (2.11), for example
`faces/f01.ysptex`, not `faces/f01.png`. The manifest records the source
path, so a script that names the source can be resolved.

### 2.8 Hashes and the pack ID

| Hash | Over | Where | Checked by |
|---|---|---|---|
| CRC-32 (zip) | each entry's data | both zip headers | `ypak verify`, any zip tool (`unzip -t`) |
| XXH64 chunk hashes | each 1 MiB chunk of an entry, seed 0 | `ysp/chunks` (entries of two or more chunks) | the reader, per chunk, on first read |
| Entry hash | XXH64, seed = entry size, over the entry's chunk hashes as little-endian u64 | 0x7379 record; manifest | the reader, per entry |
| SHA-256 | each entry's data; each source file | manifest | `ypak verify`, `ypak rebuild`, `sha256sum` on extracted files |
| Pack ID | SHA-256 of the bytes of `ysp/manifest.json` | comment; printed by `ypak info`; logged by the player | the reader at open |
| Directory hash | XXH64 of the central directory | comment | the reader at open |

- An entry of k chunks has chunk hashes c_0 to c_(k-1), where k =
  max(1, ceil(size / 2^20)) and an empty entry has one empty chunk. The
  entry hash is the same function for every size, so one code path checks
  a whole entry and a streamed one.
- The manifest lists the SHA-256 of every other entry, so the pack ID
  commits to every byte of every entry. The container bytes follow from
  the entries (2.2), and `ypak verify` checks that the container is the
  canonical one.
- None of these hashes is a signature. They find corruption, not
  forgery, as in ysp/table.h. A pack from another lab is checked for
  memory safety, not for authenticity (rig_spec 6, sandbox).

`ysp/chunks`, kind CHUNKS:

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPCHNK1" |
| 8 | 4 | version, 1 |
| 12 | 4 | log2 of the chunk size, 20 |
| 16 | 8 | n, the number of hashes |
| 24 | 8 | 0 |
| 32 | 8 n | chunk hashes, little-endian, entry by entry in pack order |

The reader verifies `ysp/chunks` and the manifest whole at open; their
records have no chunk index, whatever their size. (Changed in phase 2:
the first page gave the manifest a chunk list like any entry, which is
circular for a manifest past 1 MiB, about 2500 entries, because the
manifest lists the chunk table's hash.) A 7.2 GB film adds 55 KB.

### 2.9 Limits

| Limit | Value | Why |
|---|---|---|
| Entries | 1 to 1,048,576 | The index is 32 bytes an entry (32 MB at the limit). |
| Name | 1024 bytes, 255 a component | Portable path limits. |
| Central directory | 256 MiB | Bounds the reader's one allocation. |
| Entry and pack size | below 2^62 bytes | Offsets plus sizes never overflow int64. |
| Comment | the line of 2.6, at most 255 bytes | |

### 2.10 Hostile input

The player opens packs from other labs. The reader treats every byte as
hostile:

- The end record is searched for in the last 65,557 bytes only (22 bytes
  plus the largest comment). The zip64 locator and end record must agree
  with it.
- Every offset and size is checked in 64-bit arithmetic with overflow
  checks before use. Every extra field is walked with its length bounded
  by the record.
- Entry count, central directory size and offset agree between the end
  record, the zip64 record and the walk.
- Every rule of 2.2 to 2.7 is checked: method, flags, version needed,
  sizes, names, order (strictly ascending after entry 0, which also
  refuses duplicates), contiguity, alignment, the 0x7379 record, the
  comment.
- On first access to an entry, the reader checks its local header
  against the central record (signature, name bytes, CRC, sizes, method,
  flags, extra length) and that the padding is zeros (libzip's
  consistency check does the first part).
- Every kind's header (section 4) is bounds-checked by its view function
  before a pointer leaves the reader. The deep checks of a form stay with
  its owner (`ytb_open_view()`, `ygfx_cset_check()`, `ycol_cal` checks,
  ysp/audio.h's WAV parser, `yvid` index checks).
- `tests/fuzz/pack_fuzz.c` (libFuzzer, as `table_fuzz.c`): its input is a
  pack in memory; it opens it, looks up every name and random ones, reads
  every entry whole and through a cursor, runs every view, and repeats the
  open through the incremental path (2.12) with the bytes in pieces. The
  test also flips every bit of a small pack and truncates it at every
  length: each result is refused or reads correct data.

### 2.11 Kinds

| Id | Kind | Extension | Canonical form | Phase |
|---|---|---|---|---|
| 1 | MANIFEST | `ysp/manifest.json` | section 3 | 2 |
| 2 | CHUNKS | `ysp/chunks` | 2.8 | 2 |
| 3 | EXPERIMENT | any | opaque bytes; the manifest names the media type | 2 |
| 4 | FILE | any | opaque bytes (a license, a readme, data a script reads) | 2 |
| 5 | TABLE | `.pstb` | ysp/table.h's PSTB block | 2 |
| 6 | CALIBRATION | `.yspcal` | ysp/color.h's `ycol_cal`, 62528 bytes | 2, see open question 1 |
| 7 | FONT | `.ttf`, `.otf`, `.ttc` | the font file, unchanged | 2 |
| 8 | CURVESET | `.yspcset` | curve set format v1 (docs/gfx.md) in a container | 2 |
| 9 | GLYPHRUNS | `.ysprun` | laid-out text blocks | 2 |
| 10 | ARTWORK | `.yspart` | layers over a curve set | 2 |
| 11 | AUDIO | `.wav` | WAV at the project rate | 2 |
| 12 | SHADER | `.yspshd` | the fragment body, mode and reflection | 2 |
| 13 | TEXTURE | `.ysptex` | ysp/gfx.h planes, raw or QOI | 2 |
| 14 | VIDEO | `.mpg`, `.mp4` | the media file, CFR, closed GOPs, no B-frames | 3 (tool v0.4.0) |
| 15 | VIDEO_INDEX | `.yspvi` | ysp/video.h's index | 3 (tool v0.4.0) |
| 16 | FRAMESEQ | `.yspvseq` | ysp/video.h's frame sequence | 3 (tool v0.4.0) |
| 17 | ALPHA | `.yspalpha` | exact alpha coverage at stated sizes | not built (4.11) |
| 18 | SCRIPT | | reserved (Luau source and bytecode) | later |
| 19 | EXTENSION | | reserved (native modules by hash, rig_spec 12) | later |

The reader refuses a kind it does not know (principle 4). Adding a kind
raises the format version.

### 2.12 Reader API: `include/ysp/pack.h` (`ypak_`, `YPAK_`)

A single header in the house style: C99 floor (it builds as C99, C11 and
C++17), no dependency, private XXH64, SHA-256 and CRC-32, nothing
allocated after open. The header's manual is the reference; in short:

| Call | What it does |
|---|---|
| `ypak_open(&pk, &desc)` | Opens from `desc.path` (mapped whole), `desc.data` (memory, 8-byte aligned), `desc.reader` (bytes by range) or `desc.tail` (incremental). Returns `bool` (v0.2.0): true when open, or for `desc.tail` when it wants bytes (`pk.need_len > 0`); false with the message in `ypak_error()` and the code in `pk.error_code` |
| `ypak_feed(&pk, bytes, n)` | Incremental open: exactly the range `pk.need_off`, `pk.need_len` asked for; `YPAK_NEED` again or 0 |
| `ypak_find`, `ypak_find_n`, `ypak_at`, `ypak_first_of` | Lookup by name (binary search), by index, by kind (the EXPERIMENT) |
| `ypak_data(&pk, &e)` | The entry's bytes, zero-copy, checked and verified by the policy; NULL without a mapping or memory |
| `ypak_check(&pk, &e, bytes, n)` | Checks bytes the caller fetched: exactly `[header_off, data_off + size)` |
| `ypak_cursor_init`, `ypak_cursor_read` | A stream: `ypak_cursor_read` has the signature of `yau_reader.read` and `yvid_reader.read`; verifies each 1 MiB chunk on its first read |
| `ypak_verify_all` | Every entry now (a preflight) |
| `ypak_id`, `ypak_describe` | `sha256:<hex>`, and one log line that names the policy |
| `ypak_xxh64*`, `ypak_entry_hash`, `ypak_sha256*`, `ypak_crc32` | The hashes, for the tool and for callers |
| `ypak_cset_view`, `ypak_art_view`, `ypak_runs_view` (`ypak_runs_find`, `ypak_runs_str`), `ypak_shader_view`, `ypak_texture_view`, `ypak_qoi_decode`, `ypak_qoi_encode` | The pack's own forms (section 4): bounds of their headers only |
| `ypak_name_ok` | The reader's name rule (2.7) |

- **A path** (`YPAK_FILE`, on unless `YPAK_NO_FILE`): one read-only
  mapping of the whole file (`CreateFileMappingW` and `MapViewOfFile` at
  offset 0, the path converted from UTF-8; `mmap` on POSIX). Windows maps
  views only at 64 KB offsets, so per-entry views are not used.
- **Memory**: the whole pack, 8-byte aligned; entry data is then at least
  8-byte aligned, and 4096-aligned when the base is.
- **A range reader**: the reader copies the central directory and the
  chunk table at open. A cursor needs scratch of `min(size, 1 MiB)` and
  reads whole chunks into it.
- **Incremental** (the browser): open with the file's last bytes (64 KB
  or more); the call asks for the central directory if the tail lacks it,
  then the manifest's range, then the chunk table's. The reader does no
  I/O. A tail of the whole file opens in one call.
- **Memory use**: at open, 56 bytes an entry for the index and one flag
  byte per entry and per chunk, plus the central directory and the chunk
  table without a mapping: from `desc.arena`, or one `YPAK_MALLOC` each.
  With an arena that is too small, open returns `YPAK_ERR_FULL` with
  `pk.need`. Nothing after open (the bench counts 0).
- **Threads**: after open, any number of threads may call the lookups,
  `ypak_data()`, `ypak_check()` and their own cursors. Verified flags are
  bytes set with release semantics. `ypak_error()` holds the last failure
  of any thread.
- **Frame budget**: nothing in the reader runs on the frame thread after
  load except lookups (0.06 to 0.14 us measured, section 8). The player
  calls `ypak_data()` at load. A cursor verifies on the thread that reads,
  a decode or producer thread; one chunk costs 0.27 ms there (section 8).
- **Not built**: finding the pack before an Authenticode certificate
  table (open question 10). A pack must end at its file's end.

## 3. Reference: the manifest

`ysp/manifest.json`, kind MANIFEST, entry 0, with no chunk list (it is
hashed whole at open). UTF-8 JSON (RFC 8259) in one fixed form (section
7): keys in bytewise order at every level. ysp/json.h reads every
description and manifest and writes this form (its YJS_WRITE_PRETTY,
docs/json.md). It is the source description
(section 5) plus what the build computed. An abridged example from the
test corpus:

```json
{
  "audio": {
    "channels": 2,
    "rate": 48000
  },
  "entries": [
    {
      "crc32": "...",
      "derivation": {
        "allow_empty": false,
        "by": "ysp/table.h 0.1.0",
        "delimiter": ",",
        "op": "csv",
        "types": {
          "word": "string"
        }
      },
      "kind": "table",
      "name": "conditions.pstb",
      "sha256": "...",
      "size": 400,
      "sources": [
        {
          "path": "conditions.csv",
          "sha256": "..."
        }
      ],
      "xxh64": "..."
    }
  ],
  "experiment": {
    "entry": "experiment/main.json",
    "media_type": "application/vnd.ysp.experiment"
  },
  "format": "ysp-pack",
  "tool": {
    "libraries": {
      "layout": "ylay 0.1.0 skribidi dee63d6ba76aeddd49dea6d1b2508cf9aa391f46 sha256:ff058faa...,sha256:36ce7aec... harfbuzz 14.6.0 sheenbidi ... libunibreak ... budouxc ...",
      "lodepng": "e584a8490db1b76b81bf298c11c09d3b55d9c270",
      "ysp/audio.h": "0.2.1",
      "ysp/color.h": "0.1.0",
      "ysp/gfx.h": "0.10.5",
      "ysp/json.h": "0.1.0",
      "ysp/outline.h": "0.2.1",
      "ysp/pack.h": "0.1.0",
      "ysp/table.h": "0.1.0"
    },
    "name": "ypak",
    "version": "0.3.0"
  },
  "version": 1
}
```

Rules:

- `version` equals the comment's version. The reader checks the comment;
  `ypak verify` checks both.
- `tool.libraries`: the version of each library the tool built with.
  `layout` is there only when the pack has a GLYPHRUNS entry (from
  pack_tool.h v0.3.0), so the core and the full tool give one pack ID for
  a pack both can build (5.1).
- `entries` lists every entry except the manifest, in pack order, with
  `ysp/chunks` included. `size`, `sha256`, `xxh64`, `crc32` and `stream`
  equal the container's (`ypak verify`).
- `sources`: each input file, by its path relative to the source
  description, `/` separators, and its SHA-256. `from`: entries of this
  pack that the entry is made from (a curve set's font, glyph runs' fonts).
- `derivation`: `op` (the step), `by` (the library and version that did
  it; for glyph runs, `ylay_stamp()`'s line with every font's SHA-256), and
  the parameters: the resource's keys from the source description plus the
  defaults the tool applied, so the record is complete without the tool's
  documentation. Some values are records only and are not taken back by a
  rebuild: `faces` and `fs_type` (font), `role` (calibration), `resolve`
  and `backward` (curve set), `contract` and `validated` (shader),
  `convert` (audio), `formulas` and `text_columns` (a table from a
  workbook), `icc` (texture), `command` (video), every key of a frame
  sequence's and a video index's derivation. A VIDEO_INDEX entry is not a
  resource of the description: its video makes it again.
- Numbers are integers. A real-valued parameter is a JSON string with the
  decimal text the tool parsed (`"1e-4"`), read by ysp/table.h's
  locale-free, correctly rounded parser. The tool refuses a JSON number
  with a fraction or an exponent and says to write a string.
- Durations, where any appear, are in seconds (decided 2026-10-07).
- No time, date, host name, user name, absolute path, or compiler. These
  differ between builds of the same pack; leaving the compiler out keeps
  the pack ID the same for every build of one tool version whose entries
  agree (they do; section 9.1).
- `experiment`: the entry the player interprets and its media type
  (opaque, rig_spec 6). At most one entry has kind EXPERIMENT.
- `audio`: the project rate and channel count.
- `private: true`: the source description marked the pack private (open
  question 7).
- `license`: per entry, the SPDX expression the source description gives.

## 4. Reference: the kinds

Fields are little-endian; offsets are from the entry's start. The new
forms start with an 8-byte magic, a u32 version (1) and a u32 header size;
every section starts at a multiple of 16 and the entry's size is a
multiple of 16. "Pool" is a region of UTF-8 strings, each followed by a
NUL, named by offset and length. The views check exactly these fields;
the deep checks stay with each form's owner.

### 4.1 TABLE

The PSTB block (docs/table.md, format 1), unchanged; `ytb_open_view()` reads
it in place. Source: CSV or TSV through `ytb_open_csv()`. Parameters:
`delimiter` (`","`, `";"` or `"\t"`; default `","`), `types` (column name
to `"integer"`, `"number"` or `"string"`), `allow_empty` (default false).

Source `.xlsx` or `.xlsm` (tool v0.4.0, `op` `"xlsx"`): one worksheet
becomes CSV text (`ypt_xlsx_read()`, `pack/import.c`), and
`ytb_open_csv()` reads that text, so a workbook and the CSV that Excel
would write give the same table, with full precision. Parameter `sheet`:
the sheet's name; the first sheet when not given. The manifest records
the sheet read. The rules:

- Row 1 names the columns; its cells must be text. A column without a
  name in row 1 must be empty: a value there is refused by its cell
  reference (PsychoPy drops such a column without a word).
- A number keeps the text the workbook stores (`0.25`,
  `1.0000000000000001E-2`); ysp/table.h's parser reads it. A boolean is
  `TRUE` or `FALSE`. A formula gives its saved value; the manifest counts
  them (`formulas`). A formula with no saved value is refused: the tool
  computes no formula.
- A column whose every value is a text cell is `"string"`, so `007` keeps
  its zeros as the workbook shows it; the manifest lists these columns
  (`text_columns`). `types` in the description comes first.
- Refused by cell reference: a date or a time (a cell format with day,
  month, year, hour or second, or a built-in date format), an error value
  (`#N/A`), a cell type the reader does not know. A date in a conditions
  file is a datetime in PsychoPy and a serial number in the file; a table
  holds numbers and text, so the author picks one.
- Refused by name: a password-protected workbook (an OLE file, not a
  zip), an old `.xls`, zip64, a part that inflates past 64 MiB, a part
  whose CRC-32 or size does not match, a DOCTYPE, an entity other than
  XML's five.
- Shared strings with rich text are joined; phonetic guides (`rPh`) are
  left out; `_xHHHH_` escapes (ECMA-376) are decoded.

Measured on PsychoPy's 23 demo workbooks (9.1, phase 3): every one reads,
and every cell equals openpyxl's value.

### 4.2 CALIBRATION (decided 2026-10-09: open question 1)

The `.yspcal` file (62528 bytes), unchanged, loaded with
`ycol_cal_load()` before it is stored. The derivation records
`"role": "preview"`: a calibration in a pack is for preview and
simulation; the player never applies it on a rig. One function in the
tool (`calibration_role()`) decides this.

### 4.3 FONT (license rule decided 2026-10-09: open question 7)

The font file, unchanged: TrueType, CFF OpenType, or a collection of up
to 16 faces. Every face is opened with `yol_font_view()`. The tool reads
each face's OS/2 `fsType` and refuses a face marked restricted license
embedding (bits 0 to 3 equal 2) unless the source description sets
`"private": true`. One function in the tool (`font_license_ok()`) decides
this. The derivation records `faces` and face 0's `fs_type` (-1: no OS/2).

### 4.4 CURVESET

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPCSET1" |
| 8 | 4 | version 1 |
| 12 | 4 | header size 64 |
| 16 | 4 | units: 0 em (a font), 1 user units (artwork) |
| 20 | 4 | G, the glyph table size, below 2^24 |
| 24 | 4 | flags: bit 0, every glyph of the face is built |
| 28 | 4 | face |
| 32 | 8 | n_texels |
| 40 | 8 | n_words, a multiple of 4 |
| 48 | 8 | texels offset, 64 |
| 56 | 8 | words offset, 64 + 16 n_texels |
| 64 | | texels (4 f32 each), then words (u32): curve set format v1 (docs/gfx.md), the words starting 0x43505359, 1, G |

Parameters: `from` (the FONT entry), `face` (default 0), `glyphs`
(`"all"`, the default, or `"used"`: .notdef and the glyphs the pack's
glyph runs use from that face), `tol` (the CFF cubic tolerance in em,
default `"1e-4"`). Built resolved, with backward lists (as `pack/layout`
builds its sets), checked with `ygfx_cset_check()`. The view gives
pointers for `ygfx_cset_desc`; nothing is copied. Note that "YSPC", the
words' magic, is also the start of a `.yspcal` file; the kind, not the
bytes, says what an entry is.

### 4.5 GLYPHRUNS

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPGRUN1" |
| 8 | 4, 4 | version 1, header size 112 |
| 16 | 5 x 4 | n_fonts (at most 64), n_blocks, n_runs, n_items, n_lines |
| 36 | 4 | 0 |
| 40 | 8 x 8 | offsets of fonts, blocks, runs, items, clusters, lines, the pool; the pool's size |
| 104 | 8 | 0 |

Each section follows the one before at the next multiple of 16. Records:

- font, 16 bytes: the CURVESET entry's name and the FONT entry's name
  (pool offset and length each).
- block, 96 bytes: key, text, language (pool offset and length each);
  size, width, line height, color (f32); direction and alignment as given,
  the resolved direction (i32); n_missing (u32, always 0: the tool refuses
  text that a font lacks); w, h (f32); first run, run count, first item,
  item count, first line, line count (u32); 8 bytes 0. Blocks are sorted
  by key, keys unique.
- run, 16 bytes: font index, size (f32), first item (from the block's
  first item), count.
- item, 32 bytes: `ygfx_citem` as ysp/gfx.h takes it.
- cluster, 4 bytes per item: the codepoint offset where its cluster starts.
- line, 32 bytes: x, baseline, width, ascent, descent (f32), text start,
  text end (u32), 0.

Parameters: `fonts` (FONT entry names in fallback order, or
`{"font", "face"}`; each needs a CURVESET entry made from it), `blocks`
(`key`, `text`, `size` required; `width`, `line_height`, `color`, as
decimal strings, default `"0"`; `dir` `auto`, `ltr` or `rtl`; `align`
`start`, `end` or `center`; `lang`, BCP 47). Spans are not built yet.

### 4.6 ARTWORK

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPARTW1" |
| 8 | 4, 4 | version 1, header size 80 |
| 16 | 4 | n_layers |
| 20 | 4 | 0 |
| 24 | 4 x 8 | the viewBox: x, y, width, height (f64) |
| 56 | 8 | layers offset, 80 |
| 64 | 8 | curve set offset, 80 + 16 n_layers |
| 72 | 8 | curve set size; the entry ends with it |

A layer, 16 bytes: glyph id in the set, rgba (the file's sRGB color and
opacity, unconverted), source (1 fill, 2 stroke), element index. The
curve set is a CURVESET body with units 1. Parameter: `tol` (viewBox
units; `"0"`, the default, is 1e-4 of the viewBox's larger side).

### 4.7 AUDIO

A WAV file as ysp/audio.h reads it (RIFF, RF64 or BW64; 16-bit, 24-bit,
24 in 32, or 32-bit float; 1 channel or the project's count), at the
project rate. The tool parses it with `yau_wav_probe()` (ysp/audio.h
v0.2.1) and refuses a float sample that is not a number. Exact
conversions, recorded as `"convert"`: unsigned 8-bit to 16-bit, 32-bit
integers whose low byte is 0 to 24 in 32, 64-bit floats that are exact in
32-bit float to 32-bit float; the result is a canonical WAV (fmt and data
only). Anything else, and any rate other than the project's, is refused
with a message that says what to export. Parameter: `stream` (flag
STREAM).

Parameter `resample` (tool v0.4.0, open question 4): `"sinc_best"`
resamples a file at another rate to the project rate with libsamplerate
0.2.2's best sinc converter (`SRC_SINC_BEST_QUALITY`; BSD-2-Clause,
pinned, `tools/vendor_pack.py`), offline, so the player still never
resamples. The samples are widened to float exactly first, as ysp/audio.h
does at load, and the result is a 32-bit float WAV (`op` `"resample"`).
The derivation records `from_rate` and `clipped`, the count of samples
past full scale (a resampled peak can pass it; float holds it, and the
author sees the count). Without `resample`, a rate other than the
project's is refused as before, and the message names the parameter.
libsamplerate is compiled without FMA contraction (`-ffp-contract=off`),
so every compiler gives the same samples (the test corpus pins a
resampled file). Measured (section 9.1, phase 3): a 997 Hz sine at
44.1 kHz comes out at 48 kHz within 5.2e-8 of the ideal sine, 140 dB
below its amplitude, away from the file's first and last 200 samples.

### 4.8 SHADER

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPSHAD1" |
| 8 | 4, 4 | version 1, header size 64 |
| 16 | 4 | mode: 0 modulation, 1 color, 2 add (`ygfx_shader_mode`) |
| 20 | 4 | flags: bit 0 reads `ysp_time`, bit 1 reads `ysp_seed`, bit 2 validated by glslang (never set yet) |
| 24 | 4 | params: bit k for `ysp_param(k)` with a literal k; all ones when a k is not a literal |
| 28 | 4 | textures: bits for `ysp_tex0` to `ysp_tex2`, `ysp_utex0` |
| 32 | 4 | `YGFX_SHADER_CONTRACT` at build (ysp/gfx.h v0.10.3; 2 from v0.10.5) |
| 36 | 4 | 0 |
| 40 | 8 | XXH64 of `ygfx_shader_wrap()`'s output at build |
| 48 | 4, 4 | body offset (64), body length |
| 56 | 4, 4 | name offset (right after the body's NUL), name length |

The body and the entry's name follow, each with a NUL. The player wraps
the body itself (`ygfx_pipeline()`); it refuses another contract version
and logs a different wrap hash. Parameter: `mode` (required).

### 4.9 TEXTURE

| Offset | Size | Field |
|---|---|---|
| 0 | 8 | "YSPTEXR1" |
| 8 | 4, 4 | version 1, header size 64 |
| 16 | 4, 4 | w, h (1 to 65536) |
| 24 | 4 | format: `ygfx_format`'s value (R8 1, RG8 2, RGBA8 3, R16F 4, RGBA16F 5, R32F 6, RGBA32F 7, R16UI 8) |
| 28 | 8 | `ygfx_encoding`'s bytes as `ygfx_texture()` takes them: matrix, range, transfer, primaries, siting, chroma_nearest, 0, 0 (see the table below) |
| 36 | 4 | compression: 0 raw, 1 QOI (RGBA8 only) |
| 40 | 8 | stored bytes |
| 48 | 8 | raw bytes (w h texel size) |
| 56 | 8 | XXH64 of the raw rows, tight, row 0 at the top |
| 64 | | the data |

R16F and RGBA16F are stored as f32 (what ysp/gfx.h takes). Source: PNG,
decoded by lodepng (pinned, `tools/vendor_pack.py`). Defaults by PNG
type: 8-bit gray to R8, gray and alpha to RG8, RGB, RGBA and palette to
RGBA8 (RGB gets alpha 255), 16-bit gray to R16UI (little-endian); 16-bit
color needs `"format": "rgba32f"` (k / 65535 as f32). A `format` that
would drop or round values is refused. `encoding`: `srgb` (the default
for 8-bit), `linear` or `device`; a 16-bit PNG has no default. A `gAMA`
other than 45455 and a `cHRM` other than sRGB's are refused by name.
`compression`: `raw` (default) or `qoi`; the reader's
`ypak_qoi_decode()` checks the raw hash.

An ICC profile (a PNG's `iCCP`, a JPEG's `APP2` chunks) is accepted when
it is sRGB's and refused otherwise (`ypt_icc_srgb()`): an RGB
matrix-and-curves profile whose colorants are within 0.002 of sRGB's D50
values and whose three curves are within half a code of 255 of sRGB's at
every code. The manifest records `"icc": "srgb"`. Changed in tool v0.4.0:
v0.3.0 refused every `iCCP`, but 3 of the 4 profiles in PsychoPy's demo
photographs are sRGB's (two `sRGB IEC61966-2.1`, one compact `c2`); the
fourth, a laptop panel's profile (`Color LCD`, colorants 0.04 off, a
gamma of 1.8), is refused. The tool still converts no color: an sRGB
profile says what `"encoding": "srgb"` says.

Source JPEG (tool v0.4.0, `op` `"jpeg"`): decoded by stb_image 2.30
(pinned, `tools/vendor_pack.py`), after `ypt_jpeg_read()` checks the
frame and the segments. Gray to R8, color to RGBA8 (alpha 255); `rgba8`
for gray and `rgba32f` (k / 255, as an 8-bit PNG gives) widen exactly;
`r8` from color, `rg8` and `r16ui` are refused. `encoding` defaults to
`srgb`, so color needs `primaries` as an RGB PNG does. Refused by name:
an EXIF orientation other than 1 (programs disagree on whether to turn
the picture: save it turned, with orientation 1), CMYK, 12-bit samples,
lossless, hierarchical and arithmetic coding, two frame headers, ICC
chunks out of order or missing, a profile that is not sRGB's, a file
with no end marker, and a file too short for its frame (every 8x8 block
costs at least one bit): stb_image would decode a cut file with zeros in
place of the missing data. The fuzzer found the second case: 1,450 bytes
that claimed 512 x 62977 and decoded. JPEG decoders do not agree to the bit: on
PsychoPy's photographs stb_image and libjpeg-turbo differ by at most 3
codes, except the second-to-last column of an image with horizontally
halved chroma (up to 20 codes; 9.1, phase 3). The texture holds
stb_image's bytes, and the manifest names the decoder.

The encoding bytes are the `enc` of `ygfx_texture_desc` (ysp/gfx.h
v0.10.5), copied unchanged: the tool refuses at build every texture that
`ygfx_texture()` would refuse at load for its encoding, so the player
guesses nothing. `primaries` (`bt709` or `device`) is given only where the
table asks for it; elsewhere it is refused.

| Format | `encoding` | `primaries` | Bytes 28 to 31 | At load |
|---|---|---|---|---|
| any | `linear` | refused | 0, 0, 0, 0 | Linear values: every use (color, modulation, add, coverage, a shader's texture) |
| r8, rg8 | `srgb`, `device` | refused | RGB 1, full 2, the transfer (sRGB 3, device 1), device 1 | A color image; gray is the display's white at each level |
| rgba8, rgba32f | `device` | refused | 1, 2, 1, 1 | A color image; the codes go through the display's own transfer |
| rgba8, rgba32f | `srgb` | required | 1, 2, 3, then BT.709 2 or device 1 | A color image. `bt709`: converted through the rig's calibration, which must have chromaticities (`ygfx_texture()` refuses without one). `device`: the transfer decoded, the RGB shown as the display's |
| r16ui | `linear` only | refused | 0, 0, 0, 0 | Linear values; ysp/gfx.h decodes no transfer on R16UI: use `"format": "rgba32f"` for sRGB or device codes |

Why `primaries` is required for sRGB codes in RGB and has no default: an
untagged PNG does not say whether its RGB is BT.709's or the display's,
and an `sRGB` chunk says BT.709 while most laboratories show images as
the display's RGB. Either default would be a guess that changes the light;
the build stops and names both choices. Gray needs no choice: it has no
chromaticity of its own. The tool does not linearize 8-bit sRGB codes:
in 8 bits that merges the dark codes (the 256 sRGB codes become 183 linear
codes; codes 0 to 6 all become 0, and 7 to 17 all 1), so ysp/gfx.h
decodes the transfer per pixel instead. An
encoded texture draws as a color image only; a coverage, mask, modulation
or shader texture is built `linear`. Changed in tool v0.2.0 (2026-10-09):
v0.1.1 wrote matrix, range and transfer on every texture with primaries
0, which `ygfx_texture()` refused, and refused any encoding on R8 and RG8.

### 4.10 VIDEO, VIDEO_INDEX, FRAMESEQ (tool v0.4.0)

The design of the first page, built: the media file unchanged with flag
STREAM, its `.yspvi` index from `yvid_index_make()`, a `YSPVSEQ1` file
unchanged with flag STREAM; ffmpeg's argument vector and version
recorded. The reader did not change (a cursor feeds `yvid_desc.reader`,
the index entry `yvid_desc.index`).

A `video` resource makes two entries: the media file, named by `name`
(it ends in `.mp4` or `.mpg`, and the bytes must be that form), and
`name` + `.yspvi`, the index, as ysp/video.h finds an index beside a
file. Parameters:

- `source`: the media file, or ffmpeg's input.
- `ffmpeg`: the arguments between the input and the output. With it, the
  tool runs `ffmpeg -nostdin -hide_banner -loglevel error -y
  [ffmpeg_input] -i SOURCE [ffmpeg] -threads N OUTPUT` (no shell; each
  argument as given) and stores the output. The tool sets the encode's
  thread count N (tool v0.5.0, decided 2026-10-20; section 14): `-threads
  N` before the output, and for libx265 `pools=N:frame-threads=F`
  appended to the author's `-x265-params` (or added as `-x265-params`
  when there is none), where F is x265's own rule for a pool of N
  threads (1 below 4, 2 from 4, 3 from 8, 4 from 16, 5 from 32). Every
  encoder measured gives other bytes with another thread count, and the
  same bytes with the same count under any CPU affinity, on Windows and
  on Linux (9.1, 2026-10-20). So the count is recorded like any other
  parameter, and a rebuild runs ffmpeg with the recorded count. A thread
  setting of the author's (`-threads`, or `threads=`, `pools=`,
  `frame-threads=`, `lookahead-threads=` in `-x264-params`,
  `-x265-params` or `-x264opts`) is refused by name: `threads` is the
  only way to set the count. Tool v0.4.0 encoded on one thread (decided
  2026-10-14), 3 to 4 times slower at 1080p than 16 threads (14).

  The tool also adds the flags that keep an encoder's bytes the same on
  another CPU and another build of the same ffmpeg version (decided
  2026-10-20, open question 12): `info=0` in libx265's `-x265-params`
  (x265's info message names the build's OS and compiler and the CPU
  flags it found), and `-flags +bitexact -dct int -idct simple` for
  mpeg1video and mpeg2video (their SIMD DCTs give other bytes than the C
  ones). `+bitexact` is appended to the author's `-flags` when there is
  one. The encoder is the last `-c:v` (or `-vcodec`, `-codec:v`, `-c`,
  `-codec`); without one, ffmpeg's default for the form: mpeg1video for
  `.mpg`. The author's `info` in `-x265-params`, and `-dct`, `-idct` or a
  `-flags` value with `-bitexact` for an MPEG encoder, are refused by
  name. The command records the flags. mpeg2video is refused before the
  encode runs (decided 2026-10-20, tool v0.5.1): the video index
  (pl_mpeg) reads MPEG-1 only and misread mpeg2video's output ("GOP
  length changes"), so the step failed after the encode. Use mpeg1video
  for `.mpg`, or libx264 or libx265 for `.mp4`.

  **libx264 has no such flag.** A libx264 entry rebuilds byte for byte
  only on a CPU on which x264 takes the same instruction-set path: its
  AVX2 code and its AVX code gave different bytes (9.1, 2026-10-20), at
  every thread count, and no x264 option removes the difference. A
  rebuild on another CPU fails and names the first entry that differs.
  An author who needs a pack that any machine rebuilds encodes with
  mpeg1video (an `.mpg` entry) or libx265 (an `.mp4` entry).

  Input options in `ffmpeg_input` are not touched: decoding gives the
  same frames on any thread count. `ffmpeg_input`: input options, before
  `-i` (a raw source: `-f rawvideo -pix_fmt gray -s 64x48 -framerate
  30`). The derivation's `by` is the first line of `ffmpeg -version`, and
  `command` is the vector with `{source}` and `{output}` for the paths, so
  the manifest names no path of the machine. The same text is the index's
  note. `ypak --ffmpeg PATH` names the program; the default is `ffmpeg` on
  the PATH.
- `threads` (tool v0.5.0): the encode's thread count N, an integer from 1
  to 64, with `ffmpeg` only. Default: the building machine's logical
  processors (`GetActiveProcessorCount()`, `sysconf(_SC_NPROCESSORS_ONLN)`),
  at most 64. The derivation records the count
  used, so a pack built without `threads` rebuilds with the count of the
  machine that built it. Two machines with other counts build two packs
  with two IDs from one description; give `threads` when the pack ID must
  not depend on the machine that builds it first.
- `color`: `matrix`, `range`, `transfer`, `primaries`, `siting`, by
  ysp/video.h's names (`bt709`, `limited`, `bt1886`, `bt709`, `left`).
  What the stream states wins; a field that contradicts it is refused
  (`yvid_index_make()`). MPEG-1 defaults to BT.601, limited, BT.1886,
  BT.709, center.

The index is made by decoding every frame: MPEG-1 with pl_mpeg (pinned,
`tools/vendor_pack.py`), on every platform; H.264 and HEVC in MP4 with
Media Foundation's software decoder, on Windows only (H.264 decoding is
exact by the standard, so every decoder gives the index's hashes,
docs/video.md). ysp/video.h refuses what is not canonical, by name: a
variable rate, B-frames, a GOP that changes, an edit list, 10-bit, and
the rest of its CANONICAL FORM. The index's derivation records codec,
size, rate, frames, GOP and color.

`frameseq`: the file from `yvid_seq_open()`, unchanged, after every byte
is checked: the header and its hash, the canonical color (RGB formats as
device values, YUV formats with every field stated), each frame's header
against the index, its data against its XXH64 (QOI frames decoded), the
zero padding, and the index at the file's end. Only a frame header's
capture fields (times, trial, event) are free. The derivation records
frames, size, format, compression and rate.

### 4.11 ALPHA (not built)

Exact alpha coverage at stated sizes; the form follows the first
experiment that needs it. No experiment has needed it, so the tool
refuses `"kind": "alpha"` with that reason (tool v0.4.0).

### 4.12 EXPERIMENT and FILE

Bytes, unchanged. The experiment comes from the description's top-level
`"experiment": {"entry", "source", "media_type"}`.

## 5. Reference: the tool

### 5.1 Library and CLI

`pack/ysp/pack_tool.h`, `pack/pack_tool.c` and `pack/pack_impl.c` (prefix
`ypt_`), a C11 library in the pack tool's tree beside `pack/layout`.
`pack_impl.c` compiles the implementations of ysp/pack.h, ysp/json.h,
ysp/table.h, ysp/outline.h, ysp/gfx.h (without SDL: the shader wrapper
and the curve set check), ysp/color.h, ysp/audio.h (without miniaudio:
the WAV probe), ysp/video.h (from tool v0.4.0: `yvid_index_make()` with
pl_mpeg, and Media Foundation on Windows), ysp/timeline.h and ysp/rt.h
once, so a program that links the library defines none of them.
`pack/import.c` holds the readers of other formats (`.xlsx`, JPEG, ICC).
It also links lodepng and stb_image (each compiled as C from the fetched
file, its warnings off) and, in the full tool only, `pack/layout`
(Skribidi and HarfBuzz). It builds with MSVC, MinGW, Linux gcc and emcc,
so the designer can run the same code in the browser (the tool test
passed under node in phase 2; phase 3 was not run there, and the ffmpeg
step cannot run in a browser).

Two builds of one tool (decided 2026-10-10, review decision E). The CLI,
the library's calls and the pack format are the same in both:

| Build | CMake | Fetch once | Kinds |
|---|---|---|---|
| Core | `YSP_BUILD_PACK=ON` | `tools/vendor_pack.py --write` (lodepng, stb_image, pl_mpeg) | every kind but GLYPHRUNS: tables, calibrations, fonts, curve sets, artwork, audio, shaders, textures, video, frame sequences, files, the experiment; and verify, list, info, extract, cat, rebuild, append |
| Full | `YSP_BUILD_PACK=ON` and `YSP_BUILD_LAYOUT=ON` | also `tools/vendor_layout.py --write` | all, GLYPHRUNS too |

Configure stops with the fetch command when a file set is missing. The
core build compiles the library with `YPT_NO_LAYOUT` (a public definition
of the `ysp_pack_tool` target) and links no layout code. It refuses a
GLYPHRUNS resource by name ("glyph runs need the layout library; this
ypak was built without it"), so `ypak rebuild` of a pack with glyph runs
also fails there. `ypak` with no arguments prints "core build" on its
second line.

The manifest names the layout library (`tool.libraries.layout`) only when
the pack has a GLYPHRUNS entry (pack_tool.h v0.3.0). So for a pack without
glyph runs the two builds write the same bytes and the same pack ID, and
each `ypak` verifies and rebuilds the other's pack. CI checks this: the
tool test of each build writes `core.ysppak` from one description, and the
two files must be identical. A CURVESET with `"glyphs": "used"` takes its
glyphs from the pack's glyph runs; without glyph runs it holds .notdef only,
in both builds.

Sizes and build times: section 8.1.

`ypak`, the CLI (`pack/ypak.c`):

| Command | What it does |
|---|---|
| `ypak build SRC.json -o OUT.ysppak [--ffmpeg PATH]` | Builds a pack from a source description |
| `ypak verify PACK` | Every check: structure, canonical container, CRC-32, XXH64 tree, SHA-256 per entry against the manifest, the manifest against the 0x7379 records, the pack ID. Exit 0 only when all pass |
| `ypak list PACK` | Name, kind, size, data offset, flags |
| `ypak info PACK` | Pack ID, format, tool, experiment, project audio |
| `ypak extract PACK [NAME...] -C DIR` | Writes entries, applying the writer's name rule; does not overwrite without `--force` |
| `ypak cat PACK NAME` | One entry to standard output |
| `ypak rebuild PACK --sources DIR [-o OUT] [--ffmpeg PATH]` | 5.3; a pack appended to a program is refused (rebuild the pack it was made from) |
| `ypak append PLAYER PACK -o OUT` | Pads the player to 4096, appends the pack with absolute offsets, rewrites the comment's directory hash. The pack ID does not change. A pack already appended is refused |

Exit codes: 0, 1 with the message on standard error, 2 for a usage
error. Every write goes to a temporary file that is renamed at the end,
so a failed build leaves no pack.

### 5.2 The source description

JSON in the manifest's form, without the computed fields (`size`,
`sha256`, `xxh64`, `crc32`, `sources[].sha256`, `tool`). A resource
gives its parameters as its own keys; the manifest moves them under
`derivation`, adds `op`, `by` and the defaults, and `ypak rebuild` moves
them back. The designer
writes it; a person can too. Names default to the source path with the
kind's extension.

```json
{
  "format": "ysp-pack-source",
  "version": 1,
  "experiment": { "entry": "experiment/main.json", "source": "main.json",
                  "media_type": "application/vnd.ysp.experiment" },
  "audio": { "rate": 48000, "channels": 2 },
  "resources": [
    { "name": "conditions.pstb", "kind": "table", "source": "conditions.csv",
      "csv": { "delimiter": ",", "types": { "contrast": "number" } } },
    { "name": "fonts/NotoSans-Regular.ttf", "kind": "font", "source": "fonts/NotoSans-Regular.ttf",
      "license": "OFL-1.1" },
    { "name": "fonts/NotoSans-Regular.yspcset", "kind": "curveset",
      "from": "fonts/NotoSans-Regular.ttf", "glyphs": "all" },
    { "name": "text/pages.ysprun", "kind": "glyphruns", "fonts": ["fonts/NotoSans-Regular.ttf"],
      "blocks": [ { "key": "intro", "text": "Press a key to start.", "size": "32", "width": "800", "lang": "en" } ] },
    { "name": "art/fixation.yspart", "kind": "artwork", "source": "art/fixation.svg" },
    { "name": "audio/beep.wav", "kind": "audio", "source": "audio/beep.wav" },
    { "name": "shaders/plaid.yspshd", "kind": "shader", "source": "shaders/plaid.glsl", "mode": "modulation" },
    { "name": "faces/f01.ysptex", "kind": "texture", "source": "faces/f01.png", "compression": "qoi" },
    { "name": "faces/f02.ysptex", "kind": "texture", "source": "faces/f02.jpg", "primaries": "device" },
    { "name": "blocks.pstb", "kind": "table", "source": "blocks.xlsx", "sheet": "Conditions" },
    { "name": "video/intro.mp4", "kind": "video", "source": "raw/intro.mov",
      "ffmpeg": ["-an", "-c:v", "libx264", "-crf", "18", "-pix_fmt", "yuv420p", "-bf", "0", "-g", "30",
                 "-keyint_min", "30", "-sc_threshold", "0", "-x264-params", "open-gop=0:scenecut=0",
                 "-video_track_timescale", "30000"] },
    { "name": "video/gratings.yspvseq", "kind": "frameseq", "source": "video/gratings.yspvseq" }
  ]
}
```

The JSON reader is strict (RFC 8259, depth and size bounds, duplicate keys
refused, unknown keys refused by name) and is fuzzed like the reader.

### 5.3 The rebuild check

A methods section cites the pack ID. A reviewer checks it:

1. Get the sources and the `ypak` release that the manifest names
   (`tool.version`, `tool.build`).
2. `ypak rebuild PACK --sources DIR`. The tool reads the manifest, checks
   the SHA-256 of each source and names the first that differs, runs every
   derivation again, writes a new pack, and compares it with the original
   byte for byte. It prints `identical` or the first entry and offset that
   differ, and exits 0 only on `identical`.
3. An `exec` step (ffmpeg) runs again only when the same program version
   is present; otherwise the tool says so, checks the recorded output by
   its hash, and rebuilds everything downstream of it. Built in tool
   v0.4.0: when `ffmpeg -version` gives another first line than the
   derivation's `by`, or ffmpeg does not run, the video entry's bytes come
   from the pack (read through ysp/pack.h, so checked by their hashes),
   the report prints a `note:` line, and the index is made again from
   them. An index that cannot be made here (an MP4 index off Windows) is
   taken from the pack the same way. The source's SHA-256 is checked in
   either case. With the same version, the step runs with the
   derivation's `threads` (tool v0.5.0), whatever this machine's count.

Phase 2 CI builds one corpus pack with the MSVC, MinGW and emcc builds of
the tool and requires the same bytes (open question 2).

## 6. Reference: where each kind goes at load

| Kind | Player call | Copy at load |
|---|---|---|
| TABLE | `ytb_open_view(data, size)` | none |
| CALIBRATION | `memcpy` into a `ycol_cal`, then its check | 62528 bytes |
| FONT | `ylay_add_font(data, size, ...)`, `yol` | none |
| CURVESET, ARTWORK | view, `ygfx_cset_check()`, `ygfx_cset_make()` | GPU upload only |
| GLYPHRUNS | view; one `ygfx_crun()` per run, items from the entry | none |
| AUDIO | `yau_wav_load()` from data, or `yau_wav_open()` with a cursor | widened to float (load) or streamed |
| SHADER | view, `ygfx_pipeline()` | none |
| TEXTURE | view, `ygfx_texture()` with the stored `enc` bytes unchanged (4.9); QOI decoded first | none raw; one buffer for QOI |
| VIDEO | `yvid_desc.reader` = cursor, `.index` = the VIDEO_INDEX entry (`name.yspvi`) | streamed |
| FRAMESEQ | `yvid_desc.reader` = cursor (no index entry: the file holds its own) | streamed |

## 7. Determinism

The same sources, source description and tool version give the same
bytes; for an ffmpeg step without `threads`, so does the same logical
processor count (4.10). What makes it so:

- **Order**: entry 0 is the manifest; the rest are in bytewise name
  order; `ysp/chunks` lists hashes in that order. Work can run on many
  threads; output order does not depend on them.
- **Container**: every field of 2.2 is fixed; zip64 follows 2.3; padding
  follows 2.4; the comment follows 2.6. No timestamps.
- **Manifest text**: keys in bytewise order at every level (changed in
  phase 2 from a documented order: sorting needs no list to keep); two-space
  indentation, one member per line, an array of scalars on one line; LF
  line ends and a final LF; strings escaped only where JSON requires
  (`"`, `\`, and bytes below 0x20 as `\u00xx` in lowercase hex); other
  text as UTF-8 bytes; hex digests in lowercase; integers in decimal; no
  floats (reals are strings, section 3).
- **Paths**: relative to the source description, `/` separators; no
  absolute path enters the pack.
- **Numbers in, bits out**: decimal text is parsed by ysp/table.h's
  correctly rounded parser, never `strtod` (locale) or `printf("%f")`.
- **Pinned code**: the manifest records every library version, the
  Skribidi commit and patch hashes (packs with glyph runs only), and the lodepng commit (with
  `,tree-differs` when the fetched file is not the pinned one). It does
  not record the compiler (changed in phase 2): the builds agree, and a
  compiler in the manifest would give one pack a different ID per
  platform. A different tool version may give different bytes; `ypak
  rebuild` says so before it starts.
- **Floating point across compilers**: integer and IEEE basic operations
  (+, -, *, /, sqrt) give the same bits under MSVC, gcc and emcc when the
  build does not contract to FMA (`-ffp-contract=off`, MSVC's default
  `/fp:precise`). ysp/outline.h also calls `sin`, `cos`, `tan`, `atan2`,
  `acos`, `cbrt` and `pow` (arcs, strokes, transforms, the SVG number
  parser), whose last bit differs between C runtimes. Fonts without
  strokes use none of them in the output path except `cbrt` inside a
  `ceil`. Measured in phase 2 (9.1): MSVC, MinGW, emcc (musl) and Linux
  gcc (glibc) builds of the tool give the same bytes on a corpus with
  arcs, strokes, skews, CFF cubics, a TrueType collection and glyph runs
  in four scripts. Open question 2 stays open only for platforms not run
  (macOS, ARM).
- **Decoders** (tool v0.4.0): stb_image's JPEG path is integer
  arithmetic, and its SSE2 code gives the scalar code's bytes (measured:
  the 15 PsychoPy photographs, MinGW gcc 16.1 with and without
  `STBI_NO_SIMD`, 15 of 15 identical). pl_mpeg's MPEG-1 decode and Media
  Foundation's H.264 decode give one index per file; H.264 is exact by the
  standard (docs/video.md). The xlsx reader is integer and text only.
- **ffmpeg**: its output is the same bytes for the same ffmpeg build,
  arguments and input (measured: section 9.1, phase 3). The encoders'
  output also depends on their thread count, so the tool sets the count
  (the description's `threads`, or the machine's logical processors),
  passes it to ffmpeg and records it; a rebuild uses the recorded count
  (4.10, decided 2026-10-20). With a fixed count, the bytes did not change
  with the CPU affinity or between Windows and Linux builds of one ffmpeg
  version (9.1, 2026-10-20). They can change with the CPU's instruction
  set, at any thread count, one included. The tool adds `info=0` for
  libx265 and `-flags +bitexact -dct int -idct simple` for mpeg1video
  (4.10; mpeg2video is refused), and with them the tool's output was the same at
  every SIMD level and on Windows and Linux (9.1, section 14). libx264
  has no such flag: its AVX2 and AVX code gave other bytes, so a libx264
  entry rebuilds the same only on a CPU with the same instruction-set
  path (4.10). A rebuild with another ffmpeg takes the recorded output
  (5.3).

## 8. Costs: targets and measurements

`examples/pack/bench.c` (`pack_bench`), 2026-10-09, the Iris Xe laptop
(i7-1360P), AC, under the measurement lock (load 6 % at the start),
MinGW gcc 16.1 `-O2` and MSVC 19.44 `/O2`, medians of 9 rounds (5 for the
1 GiB pack), each in a fresh open. Packs built by `ypak` from files: 100
entries of 1 to 4000 bytes (476,598 bytes), 10,000 such entries
(46,744,114 bytes, a 4.8 MB manifest), and one 1 GiB entry.

| What | Target | gcc | MSVC | Met |
|---|---|---|---|---|
| `ypak_open()`, mapped, 100 entries | at most 0.5 ms | 0.234 ms | 0.254 ms | yes |
| `ypak_open()`, mapped, 10,000 entries | at most 20 ms | 14.7 ms | 15.6 ms | yes; most of it is the manifest's SHA-256 (4.8 MB at the probe's 0.31 to 0.38 GB/s is 13 to 16 ms) |
| `ypak_open()`, mapped, 1 GiB pack | | 0.163 ms | 0.148 ms | |
| Central directory walk and checks | at most 0.2 us an entry | not timed apart | | the 10,000-entry open less its SHA-256 leaves at most about 2 ms |
| `ypak_find()`, shuffled names | median at most 1 us | 0.06 to 0.14 us | 0.06 to 0.14 us | yes |
| Entry hash, memory already touched | at least 10 GB/s | 11.7 to 17.1 GB/s | 11.6 to 16.5 GB/s | yes (MSVC after the load fix, 9.1) |
| `ypak_verify_all()`, mapped first touch, 1 GiB | at least 2 GB/s | 3.45 GB/s | 3.65 GB/s | yes |
| The same, 10,000 small entries | at least 2 GB/s | 1.51 GB/s | 1.20 GB/s | no: about 2 KB an entry, so a page fault and a header check per entry dominate; 16 to 21 ms for the whole pack |
| The same, 100 small entries | | 1.84 GB/s | 1.26 GB/s | 0.14 to 0.20 ms |
| From the SSD, cold | at least 3 GB/s | not run in phase 2 | | the phase 1 probe: 3.22 GB/s read and XXH64 on one thread |
| One 1 MiB chunk through a cursor over a range reader (`fread`, OS cache) | at most 0.5 ms | 0.277 ms | 0.270 ms | yes |
| Browser open, central directory in the 64 KB tail | 2 range requests at most for the directory | 0 for the directory | | the incremental test: the tail, then the manifest's range, then the chunk table's (3 requests in all) |
| Size overhead | at most 4096 + 128 bytes an entry, plus 8 bytes a MiB | 2,182 to 2,190 bytes an entry | | yes |
| Fuzzing | 1 hour of libFuzzer with ASan, no finding | 28,706,212 inputs in 3,601 s, no finding (9.1) | | yes |
| Allocations after open | 0 | 0 | 0 | yes |

Frame budget: after load, a lookup (0.06 to 0.14 us) is the only reader
call a frame needs. Verifying a whole entry happens at load; a chunk
through a cursor costs 0.27 ms on the decode or producer thread.

### 8.1 The core and the full tool: size and build time

2026-10-10, the same laptop (i7-1360P, 16 logical processors), AC, under
the measurement lock (load 12 % at the start). Each round configures a
new build folder and builds the `ypak` target alone (examples and tests
off), Release, `--parallel 16`; 3 rounds, core and full alternating. The
fetched sources were already on disk, so no time is download time.

| | Core (`YSP_BUILD_PACK`) | Full (`YSP_BUILD_PACK`, `YSP_BUILD_LAYOUT`) |
|---|---|---|
| Sources fetched | lodepng, 3 files (0.4 MB) | also the layout trees (about 8 MB) |
| MSVC 19.44, configure | 10.2 to 13.6 s | 10.1 to 20.7 s |
| MSVC 19.44, build `ypak` | 7.2 to 17.8 s | 35.7 to 44.8 s |
| MSVC 19.44, configure and build | 18.2 to 28.0 s | 45.8 to 65.5 s |
| `ypak.exe`, MSVC Release | 446,976 bytes | 1,853,440 bytes |
| `ypak.exe`, MinGW gcc 16.1 Release (`-O3`), not stripped | 1,135,016 bytes | 3,399,699 bytes |

The core `ypak` from MSVC and from MinGW and the full `ypak` from
both compilers wrote the same `core.ysppak` (the tool test's description
without glyph runs), byte for byte.

### 8.2 The tool's throughput per kind (tool v0.4.0)

`tests/pack/throughput.py`, 2026-10-13, the same laptop (i7-1360P), AC,
under the measurement lock (load 17 % at the start, 16 % at the end),
`ypak.exe` MSVC 19.44 Release, core build. Each row is one `ypak build` of
every file of that kind, so it includes the process start, the source's
SHA-256, the derivation, the entry hashes and the write; median of 5
rounds (3 for the panorama), min to max in brackets. The corpus is
realistic: PsychoPy's demo workbooks and photographs (section 9.1, phase
3), their sheets saved as CSV by the tool's reader, the photographs saved
as PNG by Pillow, two H.264 clips made as `tests/media/make_video_clips.sh`
makes them (1280 x 720 and 1920 x 1080, 300 frames each, 30 fps) and one
MPEG-1 clip (1280 x 720, 300 frames), two 1920 x 1080 frame sequences of
60 frames (RGBA8 as QOI, 5.9 MB; I420 raw, 186.8 MB), and two WAV files at
48 kHz (60 s of 16-bit stereo, 10 s of float stereo).

| Kind | Files | Source | Pack | Time | Source rate | Per file |
|---|---|---|---|---|---|---|
| TABLE from `.xlsx` | 23 | 0.6 MB | 0.1 MB | 0.051 s (0.047 to 0.053) | 12.2 MB/s | 2.2 ms |
| TABLE from CSV (the same sheets) | 23 | 0.02 MB | 0.1 MB | 0.044 s (0.035 to 0.047) | | 1.9 ms |
| TEXTURE from JPEG | 14 | 2.6 MB | 30.1 MB | 0.270 s (0.245 to 0.311) | 9.7 MB/s; 111 MB/s of texels | 19.3 ms |
| TEXTURE from PNG (the same photographs) | 14 | 11.4 MB | 30.1 MB | 0.415 s (0.354 to 0.429) | 27.6 MB/s | 29.6 ms |
| TEXTURE from JPEG, the 10240 x 5120 panorama | 1 | 14.1 MB | 209.7 MB | 1.228 s (1.208 to 1.453) | 11.5 MB/s; 171 MB/s of texels | |
| TEXTURE from PNG, the same panorama | 1 | 77.6 MB | 209.7 MB | 2.615 s (2.601 to 2.634) | 29.7 MB/s | |
| VIDEO and its index (copy; every frame decoded) | 3 | 31.7 MB | 31.7 MB | 3.342 s (3.296 to 3.519) | 9.5 MB/s | 3.7 ms a frame |
| FRAMESEQ (every frame checked) | 2 | 192.7 MB | 192.7 MB | 1.741 s (1.410 to 1.747) | 110.7 MB/s | |
| AUDIO (copy) | 2 | 15.4 MB | 15.4 MB | 0.159 s (0.153 to 0.171) | 96.5 MB/s | |
| TEXTURE from PNG as QOI (the 14 photographs; second session) | 14 | 11.4 MB | 13.6 MB (45 % of raw) | 0.331 s (0.324 to 0.333); raw 0.354 s (0.343 to 0.410) in the same session | | |
| AUDIO resampled 44.1 to 48 kHz, 60 s of 16-bit stereo (second session) | 1 | 10.6 MB | 23.1 MB | 2.043 s (2.023 to 2.061); a 48 kHz copy 0.107 s | 1.3 million frames a second, 29 times real time | |
| VIDEO through the ffmpeg step: PsychoPy's demo movie, 640 x 480, 173 frames, x264 on one thread, then its index (second session, 3 rounds) | 1 | 0.8 MB | 0.5 MB | 1.651 s (1.615 to 1.812) | | |

The second session (QOI, resampling, ffmpeg) ran later the same night
under the lock, with the full build's `ypak.exe`; its load was not
recorded. QOI answers the first half of open question 6: on these
photographs it stores 45 % of the raw bytes and builds no slower; its
decode at load was not timed.

What this shows: a conditions workbook costs 2.2 ms, 0.3 ms more than its
CSV; neither matters beside the process start. A JPEG photograph builds
1.5 times faster than the same photograph as PNG; the PNG is 4.4 times
larger to read, hash and inflate (the parts were not timed apart). The
index dominates video: it decodes every frame on one thread to hash it,
3.7 ms a frame over the three clips. Frame sequences and copies are bound
by the two SHA-256 passes (the source's and the entry's) in portable C at
0.31 to 0.38 GB/s (9.1): for the 193 MB of frame sequences that is about
1.1 s of the 1.7 s, for the 15 MB of audio about half of the 0.16 s.

## 9. Explanation

### 9.1 Measurements

**Checksums**, 2026-10-08, i7-1360P, AC, under the measurement lock (load
6 % and 10 % at the start), 5 rounds, GB/s median (min to max). Native:
MinGW gcc 16.1 `-O2`, xxHash 0.8.3 (`xxhash.h`) for XXH64 and XXH3, a
portable SHA-256, Windows CNG for SHA-256 with the SHA extensions.
WebAssembly: emcc 6.0.10 `-O2`, node 24.19, 256 MiB. Python 3.14.3
(zlib-ng 1.3.1) for the vectorized CRC-32.

| Hash | Native, 1 GiB in memory | WebAssembly | WebAssembly `-msimd128` |
|---|---|---|---|
| reading the bytes (a sum of u64) | 14.2 to 18.5 | | |
| CRC-32, byte table | 0.53 to 0.60 | | |
| CRC-32, slice-by-8 | 2.49 to 3.03 | 1.50 (1.13 to 2.33) | 1.81 (1.81 to 1.89) |
| CRC-32, zlib-ng (carry-less multiply) | 13.82 (12.09 to 14.53) | | |
| XXH64 | 10.2 to 11.7 | 8.77 (3.78 to 9.73) | 11.74 (3.85 to 12.11) |
| XXH3-64, SSE2 | 13.07 to 15.08 | 7.71 (4.39 to 9.85) | 12.72 (5.09 to 13.33) |
| XXH3-64, AVX2 | 15.90 (13.24 to 20.32) | | |
| XXH3-64 over 1 MiB chunks, then the list | 12.9 to 16.2 | | |
| SHA-256, portable C | 0.31 to 0.38 | 0.21 (0.04 to 0.27) | 0.27 (0.06 to 0.27) |
| SHA-256, Windows CNG | 1.96 to 2.31 | | |
| SHA-256, Python hashlib (OpenSSL) | 2.27 (2.09 to 2.36) | | |

Native rows that show a range give the medians of the separate runs.

| From the file, 1 GiB, native | GB/s median (min to max) |
|---|---|
| unbuffered read, 4 MiB blocks | 3.80 to 4.05 |
| unbuffered read, then XXH3 on the same thread | 3.36 to 3.83; 3.62 (3.57 to 3.65) in the XXH64 run |
| unbuffered read, then XXH64 on the same thread | 3.22 (2.93 to 3.34) |
| in the cache, mapped, XXH3 at first touch | 2.41 to 2.57 |
| in the cache, mapped, XXH64 at first touch | 2.20 (1.88 to 2.75) |

What this shows: SHA-256 in portable C is 10 times slower than the SSD,
so it cannot run at load on large entries. XXH64 and XXH3 are both above
the SSD and the page-fault rate; in the same run, XXH3's lead shrinks to
12 % on the cold path (3.62 against 3.22) and 10 % on the mapped path
(2.41 against 2.20). CRC-32 is fast only with carry-less
multiply, which needs per-architecture code and has none in WebAssembly.
ysp/video.h reported 4.3 to 5.3 GB/s for its own XXH64; the reference
implementation gives 10.2 to 11.7 here, so that implementation has room
(not measured here on the same input).

**Zip readers**, 2026-10-08. Packs written by a probe with the layout of
section 2: six entries (14 bytes to 100 KB, one empty, one with a
non-ASCII name), each aligned by 0xD935, the 0x7379 record in the
central directory, zero DOS time, the comment of 2.6. Every reader got
the same names, sizes and SHA-256 values as the input.

| Reader | Plain | zip64 on every entry | 4.4 GB zip64 entry | Appended, relative offsets | Appended, absolute offsets |
|---|---|---|---|---|---|
| Python 3.14 `zipfile` (with `testzip`) | ok | ok | ok | ok | ok |
| Info-ZIP UnZip 6.00 `-t` | ok | ok | ok | ok, warning, exit 1 | ok |
| bsdtar 3.8.8 (Windows `tar.exe`), list | ok | ok | ok | ok | ok |
| bsdtar 3.8.8, from a pipe | | | ok | | |
| Java 17 `ZipFile` | ok | ok | ok | ok | ok |
| Java 17 `ZipInputStream` (streaming) | ok | ok | ok | no entries (expected) | no entries (expected) |
| .NET Framework 4.8 `ZipFile` | ok | ok | ok | refused | ok |
| .NET 10 `ZipFile` | ok | ok | ok | refused ("Number of entries expected in End Of Central Directory ...") | ok |

The 4.4 GB pack also needed the zip64 end record and locator (its
central directory starts past 4 GiB).

**Phase 2: tests.** 2026-10-08, the same laptop.

| Build | Reader test | Tool test | Other |
|---|---|---|---|
| MSVC 19.44, `/W4 /WX` | 441 checks, 0 failed | 115 checks, 0 failed | full ctest 65 of 65 (with `YSP_BUILD_LAYOUT`, `YSP_BUILD_PACK`) |
| MinGW-w64 gcc 16.1, `-Wall -Wextra -Wpedantic -Wshadow -Werror` | 441, 0 failed | 115, 0 failed | full ctest 65 of 65; C99 compile check |
| emcc 6.0.10, node 24.19 | 408, 0 failed (no file source under node) | 115, 0 failed | C11 and C++17 compile checks; ysp/audio.h and ysp/gfx.h tests pass |
| Linux gcc 11.4 (WSL2, glibc) | 441, 0 failed; again under ASan and UBSan as C11 and as C++17, no report | 115, 0 failed | |

The reader test refuses all 135,240 single-bit flips and all 16,905
truncations of a two-entry pack, and refuses each of 29 rule faults for
its own reason (`YPAK_ERR_FORMAT` for a rule, `YPAK_ERR_CORRUPT` for a
hash, `YPAK_ERR_VERSION` for format 2).

**Phase 2: mutations.** `tests/mutate/pack.toml`, 36 mutants (comment and
end records 4, central directory 11, local headers 3, hashes 7, names 3,
cursors, checks and the arena 3, views 5). The first run killed 31; the 5
survivors were gaps in the test, each closed: d-11 (the chunk index
check) needed the fault test to demand the fault's own error code,
because a chunk-list hash refused the same pack a step later; k-02 needed
a fetched range one byte too long; v-01, v-03 and v-04 needed view inputs
that break only the rule under test. Second run: 36 of 36 killed.
`tests/mutate/audio.toml`: 4 of 4. `gfx.toml` v10-07: killed.

**Tool v0.2.0: mutations of the TEXTURE encoding, by hand.** The runner
in `tests/mutate` builds one test source against copied headers and
cannot build the tool, so these 8 mutants of `pack/pack_tool.c` were
applied one at a time, `test_pack_tool` rebuilt (MSVC 19.44) and run,
and the file restored (2026-10-09). All 8 killed: linear stores the
encoding bytes (8 failing checks); gray and device primaries left 0 (5);
sRGB RGB without `primaries` taken (the test crashed on the missing
name); `bt709` stored as device (4); r16ui with a transfer taken (1); the
primaries not recorded for the rebuild (1: the rebuild refuses); an
unknown primaries name taken (11); `primaries` taken where none apply
(32). Counts include checks that fail after the first one.

**Phase 2: fuzzing.** `tests/fuzz/pack_fuzz.c` under MSVC 19.44
`/fsanitize=address /fsanitize=fuzzer`, from 23 seeds (three packs built
by `ypak`, one of them appended to a program, as targets 0 to 2, and their
pack-own entries as view inputs): 28,706,212 inputs in 3,601 s, no crash,
no timeout, no leak; 1,622 coverage features, 750 corpus files, peak RSS
534 MB. The 750 files replayed on Linux gcc 11.4 under ASan and UBSan, as
C11 and as C++17: no report.

**Phase 2: the same bytes from every build.** See open question 2: the
MSVC, MinGW, emcc and Linux gcc builds of the tool give the same bytes
on a 5.2 MB test corpus and a 208 MB system-font corpus.

**Phase 2: zip readers on packs the tool built.** `tests/pack/compat.py`,
the six readers of the phase 1 table, every entry's SHA-256 against the
manifest: the test corpus (22 entries), a small pack, its copy appended
to a program (absolute offsets), and a 4,300,046,978-byte pack of a
4.3 GB entry followed by a small one (zip64 by size, by local header
offset and by directory offset). Every reader agreed on every pack, Java's
streaming reader excepted on the appended copy (it cannot start inside a
program). `ypak verify` of the 4.3 GB pack: 18 s, every check passed.

**Found while building.** MSVC compiles the reader's byte-by-byte 64-bit
load as eight loads; XXH64 then ran at 5.1 to 6.8 GB/s against gcc's 11.5
to 18. A `memcpy` load under `_MSC_VER` gave 11.6 GB/s (a `_rotl64` rotate
changed nothing). ysp/video.h's XXH64 uses the same byte loads and
reported 4.3 to 5.3 GB/s; its owner may want the same change (not
measured there). One `fwrite` of 4.3 GB under MinGW's msvcrt ran for
minutes and wrote 5 MB; the tool now reads and writes in 16 MiB pieces.

**Phase 3: the readers against other readers**, 2026-10-13, the same
laptop (i7-1360P), on PsychoPy's demo files (`psychopy/psychopy` branch
`release` at `051482a`: 23 workbooks, 16 JPEG files, one MP4).

- Workbooks: all 23 read. Every cell of each first sheet's named columns
  (2,305 cells) equals openpyxl 3.1's value with `data_only=True`: text
  equal, numbers equal as doubles, booleans `TRUE` or `FALSE`, no row
  more or less.
- JPEG: 14 of the 15 distinct photographs decode; the 15th
  (`stimuli/beach.jpg`) carries a laptop panel's ICC profile (`Color LCD`,
  rXYZ 0.3949 0.2251 0.0296, a gamma of 1.8) and is refused. Against
  Pillow 12.3 (libjpeg-turbo): at most 3 codes apart on 13 files, 0.005 to
  0.14 codes on average; on `canal.jpg` (progressive, chroma halved across
  only) up to 20 codes, all in the second-to-last column (224 of 786,432
  texels above 3 codes): the two decoders treat the right edge of
  horizontally halved chroma differently. 3 of the 4 profiles in the set
  are sRGB's: two `sRGB IEC61966-2.1` (a 1024-entry curve, 0.00 codes from
  sRGB's), one compact `c2` (26 entries, 0.11 codes).
- stb_image's SSE2 code against `STBI_NO_SIMD` (MinGW gcc 16.1 `-O2`):
  the same bytes on all 15 files.
- The MP4 (`stimuli/jwpIntro.mp4`, 173 frames at 2997/100 fps, no color
  stated) is refused as given: its frame times fit no constant rate.
  Through the ffmpeg step (x264, `-vf fps=30000/1001`, stated BT.601
  color, one thread) it builds in 1.65 s (8.2), and its index reads 173
  frames at 30000/1001.

**Phase 3: ffmpeg's bytes.** ffmpeg N-122953-g7b15039cdb-20260223
(gcc 15.2), the movie above through libx264 `-preset medium -crf 18`,
twice for each thread count: each count gives the same bytes run to run;
`threads=1`, `threads=4`, `threads=8` and the default (16 logical
processors here) give four different files.

2026-10-14, the encoders the tool is likely to be asked for, 120 frames
of the same movie, twice each with the default threads and with
`-threads 1`, `-threads 4`, `-threads 8`:

| Encoder | Run to run | Thread counts | With `-threads 1` |
|---|---|---|---|
| libx264 | same | default, 4 and 8 all differ | same run to run; equal to `-x264-params threads=1` |
| libx265 | same | default, 1 and 8 differ (4 happened to equal the default) | depends still on x265's own pool: `pools=1`, `pools=4` and the default pool give three files; `pools=1:frame-threads=1` is pinned, `lookahead-threads` changes nothing |
| mpeg1video | same | default, 1, 4 and 8 all differ | same run to run |
| mpeg2video | same | default, 1, 4 and 8 all differ | same run to run |

Hence the rule of 4.10 (decided 2026-10-14): `-threads 1` for every
encode, and `pools=1:frame-threads=1` for libx265. The tool test's
MPEG-1, H.264 and HEVC clips are the same bytes on a second build, the
HEVC clip's command shows the pools appended to the author's
`-x265-params`, and `ypak rebuild` with this ffmpeg and with no ffmpeg
gives the pack byte for byte.

**Phase 3: the thread rule's mutants, by hand** (the runner cannot
build the tool), 2026-10-14, MSVC 19.44, `test_pack_tool`: no
`-threads 1` (killed, 2 checks), libx265's pools not merged into the
author's `-x265-params` (killed: ffmpeg's second `-x265-params` replaces
the author's, B-frames return and the index refuses them), the author's
thread settings taken (killed, 3 checks). 3 of 3.

**Phase 3: the cost of one thread**, 2026-10-14, under the measurement
lock (load 41 % when it ended; other workers' jobs started after it), the
same ffmpeg, median of 3 encodes (min to max), the encode alone:

| Source | Encoder | Default threads | One thread | Ratio |
|---|---|---|---|---|
| PsychoPy's demo movie, 640 x 480, 173 frames | libx264 medium, crf 18 | 0.76 s (0.73 to 0.92) | 1.34 s (1.19 to 1.56) | 1.8 |
| the same | libx265 medium, crf 20 | 1.69 s (1.52 to 1.79) | 3.14 s (2.42 to 3.59) | 1.9 |
| the same | mpeg1video, q 3 | 0.23 s (0.22 to 0.25) | 0.33 s (0.32 to 0.57) | 1.4 |
| testsrc2, 1920 x 1080, 300 frames (10 s) | libx264 medium, crf 18 | 2.89 s (2.89 to 3.45) | 29.43 s (24.23 to 31.23) | 10.2 |
| the same | libx265 medium, crf 20 | 13.45 s (11.48 to 16.59) | 73.80 s (66.66 to 81.92) | 5.5 |
| the same | mpeg1video, q 3 | 0.83 s (0.81 to 0.89) | 1.36 s (1.31 to 1.43) | 1.6 |

What this costs: short clips at stimulus sizes pay under a factor of 2;
1080p H.264 pays 10 times, about 3 s of encode a second of film here, so
a two-hour 1080p film would take about 6 hours to pack once. The
encode runs once per pack; a rebuild with another ffmpeg takes the
recorded output instead. A faster `-preset` is the author's lever, and it
is recorded like any argument.

**2026-10-20: a fixed thread count on other machines** (tool v0.5.0,
section 14). Does an encode with N threads give the same bytes whatever
machine runs it? BtbN's static builds of ffmpeg n9.0.2-28-g669f5ab083
(2026-10-19; libx264 core 165, x265 4.3+49), the Windows and the Linux
build of one commit, on the i7-1360P (4 P-cores with 2 threads each, 8
E-cores; AVX2, no AVX-512). Sources: PsychoPy's demo movie through `-vf
fps=30000/1001` (640 x 480, 173 frames), and `testsrc2` at 1920 x 1080,
300 frames. Encoders as the tool runs them: libx264 `-preset medium -crf
18`, libx265 `-preset medium -crf 20` with `pools=N:frame-threads=F`
(4.10), mpeg1video and mpeg2video `-q:v 3`; `-threads N` for every one.
N = 1, 4, 8, 16. Each configuration 3 times, the SHA-256 of the output
file compared: 936 encodes, every one under the build guard.

| Configuration | What it stands for | libx264 | libx265 (`info=0`) | libx265 (its default info message) | mpeg1video, mpeg2video | the same with `-flags +bitexact -dct int -idct simple` |
|---|---|---|---|---|---|---|
| Affinity 2 logical processors (one P-core), set before the process runs | a smaller machine | same | same | same | same | same |
| Affinity 4 E-cores | a slower machine | same | same | same | same | same |
| WSL2, the Linux build, 8 logical processors | another OS, compiler and C library | same | same | differs at every N | same | same |
| x264 limited to AVX (no FMA3, BMI2, AVX2), x265 `asm=avx`, ffmpeg's flags to AVX | a CPU without AVX2 | differs at N = 1, 4, 8 on the movie and at N = 16 at 1080p; same in the other 4 | same | differs at every N | same | same |
| x264 `asm=0`, x265 `asm=0`, ffmpeg `-cpuflags 0` | a CPU whose code paths differ (another architecture) | differs at every N | same | differs at every N | differs at every N | same |

"Same": all 3 repeats equal the unrestricted Windows encode with the same
N, for both sources (x265 with its info message: the movie only). Every
configuration gave the same bytes in all 3 repeats, and the 4 values of N
gave 4 different files for every encoder but libx265, whose N = 4, 8 and
16 gave one file (frame threads 2, 3 and 4 code alike on these clips).

What this shows:

- **The thread count adds no dependence on the machine.** With N fixed,
  no encoder changed its bytes with the processors it ran on or between
  Windows and Linux, at any N. Where a configuration differed, it differed
  at N = 1 too. One thread for every encode (tool v0.4.0) therefore bought
  nothing that N threads lose; 4.10 now records N.
- **libx264 depends on the instruction set** at every N. Its AVX2 code
  gives other bytes than its AVX code (the decoded frames differ too, so
  it is the coding, not a header); x264 has no option that makes its
  output independent of the CPU. A one-thread probe on the movie located
  it: the masks with and without FMA3 at the AVX level gave one file, and
  the AVX2 mask (with BMI2) another; SSE4.2 equaled AVX, SSE2 equaled no
  SIMD. AVX-512 (AMD Zen 4 and 5) could not be run here.
- **libx265's coded pictures do not depend on the instruction set** (the
  same bytes with every SIMD path off), but its info message (an SEI)
  holds `[Windows][GCC 16.2.0]` and `cpuid=1111039`, so the file differs
  with the build's OS and the CPU unless the author sets `info=0`.
- **mpeg1video and mpeg2video** gave one file from SSE2 up to AVX2 (the
  levels x86-64 CPUs have) and another with every SIMD path off: the SIMD
  forward DCT and IDCT are not the C ones. `-flags +bitexact -dct int
  -idct simple` gave one file at every level, 0 included.

Decided by the user the same day (open question 12): the tool adds
`info=0` and the three MPEG flags itself (4.10); libx264 stays as it is,
documented.

**2026-10-20: the tool's own output with the forced flags** (tool v0.5.0,
the same ffmpeg builds and sources; the 1080p source as a lossless FFV1
file). `ypak build` of mpeg1video (`-q:v 3 -g 15 -bf 0`) and libx265
(`-preset medium -crf 20`, no B-frames, a fixed GOP) entries with
`"threads"` 4 and 16, each 3 times; the same with every SIMD path off
(`"-cpuflags", "0"` in `ffmpeg_input`, and `asm=0` in libx265's
parameters), whose video entry (`ypak cat`) was compared with the
first's; then `ypak rebuild` of each first pack 3 times on WSL2 with the
Linux builds of ypak (gcc 11.4) and ffmpeg, 8 logical processors against
the recorded 4 and 16:

| Entry | N | Windows, 3 builds | SIMD off, 3 builds | WSL2 rebuild, 3 times |
|---|---|---|---|---|
| mpeg1video, the movie | 4, 16 | one file per N | the same files | `identical` |
| mpeg1video, 1080p | 4, 16 | one file per N | the same files | `identical` |
| libx265, the movie | 4, 16 | one file (N = 4 and 16 code alike) | the same file | `identical` |
| libx265, 1080p | 4, 16 | one file (likewise) | the same file | `identical` |

mpeg2video could not be packed: its output is not MPEG-1, and the
index (pl_mpeg) misreads it ("GOP length changes"). Tool v0.5.1 refuses
it before the encode (4.10).
Its flags stand on the ffmpeg-level measurement above (one file at every
SIMD level and on WSL2, at every N). The hand test of section 14 also
passed again with these flags (`identical` for its MPEG-1, libx264 and
libx265 packs on WSL2).

**Phase 3: resampling.** A 997 Hz sine at 44.1 kHz (float, amplitude
0.5) through `"resample": "sinc_best"` to 48 kHz: 48,000 frames, the
largest difference from the ideal sine 5.2e-8 (140 dB below the
amplitude) between frame 200 and the last 200 frames (MSVC 19.44).

**Phase 3: tests**, 2026-10-13.

| Build | Tool test | Import test | Notes |
|---|---|---|---|
| MSVC 19.44 `/W4 /WX`, full (`YSP_BUILD_LAYOUT`) | 255 checks, 0 failed (2026-10-14) | 89, 0 failed | video through ffmpeg: MPEG-1, H.264 and HEVC in MP4 |
| MSVC 19.44 `/W4 /WX`, core | 246, 0 failed | 89, 0 failed | the same `core.ysppak` as the full build, byte for byte |
| Linux gcc 11.4 (WSL2), ASan and UBSan, core | 242, 0 failed | 89, 0 failed | MPEG-1 through ffmpeg 4.4.2; no MP4 index off Windows; the corpus pack, manifest included, is the MSVC core build's byte for byte (`sha256:013f3079...`), the resampled WAV and the sine's error too |
| MinGW gcc 16.1, `-Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion` | | 89, 0 failed | `pack/import.c` without a warning |

**Phase 3: mutations.** `tests/mutate/import.toml`, 38 mutants of
`pack/import.c` (zip 9, XML 5, cells 11, JPEG 10, ICC 3), MinGW gcc 16.1:
37 of 37 killed on the first run, after the test gained a repeated row
number, a comma without a quote and a column of numbers and text while
the list was written; the 38th (j-10, the frame-size check the fuzzer
led to) killed too.

**Phase 3: fuzzing.** `tests/fuzz/import_fuzz.c` under MSVC 19.51
`/fsanitize=address /fsanitize=fuzzer`, from 97 seeds (PsychoPy's
workbooks and photographs, their ICC profiles, the tool test's workbooks
and JPEG files), in 10-minute pieces under the build guard. A first run
without a length bound (about 150,000 inputs) found one timeout: 1,450
bytes whose frame claimed 512 x 62977, which the reader passed and
stb_image decoded (0.3 s without sanitizers, 13 s under them). The
reader now refuses a file too short for its frame (4.9, mutant j-10),
and the target decodes frames up to 4 megapixels only. Then, with
`-max_len=40000`: 922,080 inputs in 3,600 s, no crash, no timeout, no
leak; 6,272 coverage features, 3,499 corpus files, peak RSS 519 MB. The
3,499 files replayed on Linux gcc 11.4 under ASan and UBSan: no report.

### 9.2 Why zip64 only where needed

Two hours of 1080p H.264 at 8 Mbit/s is 7.2 GB; MPEG-1 for pl_mpeg needs
several times that rate; a frame sequence of raw frames is larger still.
So zip64 is required. Writing it only where a value overflows keeps the
plain form for the common pack, which every reader handles, and the rule
is a function of the sizes, so it is deterministic.

### 9.3 Why padding in the extra field, and why 0xD935

A zip reader that walks local headers from the start (Java's
`ZipInputStream`, bsdtar from a pipe) needs each header right after the
previous entry's data. Padding inside the extra field keeps that true;
padding between entries would not. zipalign used anonymous zero padding
first, which is not a valid extra field; Android's later tools write the
0xD935 record, which APPNOTE lists. Apache Commons Compress has its own
(0xA11E); 0xD935 was chosen because Android's tools made it common.

### 9.4 Why absolute offsets in the appended copy

Measured (9.1): both .NET readers refuse a zip whose offsets are relative
to its own start when bytes precede it, and Info-ZIP warns with exit
code 1. With absolute offsets (Info-ZIP's `zip -A`), every reader that
can read a prefixed zip at all reads it, with no warning. Only the central
directory and the end records change; the entries and the manifest do
not, so the pack ID stays. The reader accepts both forms: it computes the
zip start as (end record position - directory size - directory offset),
as Python's `zipfile` and Java's `ZipFile` do.

### 9.5 Why XXH64, SHA-256 and CRC-32, each for one job

- CRC-32 is in the zip format and lets any zip tool test the pack. Fast
  CRC-32 needs carry-less multiply per architecture and has no
  WebAssembly path (1.5 to 1.8 GB/s there), so the runtime does not use it.
- SHA-256 is the hash that a reviewer can compute with `sha256sum`, that
  `third_party/MANIFEST.sha256` and `ylay_font_info` already use, and
  that resists deliberate collisions, which matters for a citation. At
  0.31 to 0.38 GB/s in portable C it is a build-time and verify-time
  hash. The tool may use the SHA extensions where present (CNG: 1.96 to
  2.31 GB/s); the result is the same by definition.
- XXH64 is the run-time check. It runs at 10 to 12 GB/s natively and in
  WebAssembly, above the SSD (4.0 GB/s) and the page-fault rate of a
  mapped first touch (2.2 to 2.6 GB/s), so verification costs little
  beyond the read that load does anyway. XXH3 is 10 to 12 % faster on
  those paths, but its fast paths are per instruction set (SSE2, AVX2,
  NEON, WebAssembly SIMD) and several hundred lines more to fuzz;
  ysp/video.h already chose XXH64 for its container and index. A future
  format version can change the function; the version field is there for
  that.
- The tree over 1 MiB chunks lets a cursor verify a streamed film chunk
  by chunk (0.1 ms a chunk at 10 GB/s, about 0.3 ms cold), the way a
  browser fetches it, while one entry hash still covers the whole entry.
  Unreal's IoStore and Android's APK Signature Scheme v4 (fs-verity) hash
  chunks for the same reason.

### 9.6 Why a JSON manifest and a binary directory record

The manifest is for people and tools: a reviewer reads it in a text
editor, Python and the browser parse it with no ysp code, and the designer
writes it. The reader header must stay small, dependency-free and safe
on hostile bytes, and it only needs per entry a kind, an offset, a hash
and a chunk index. Those go in the central directory, which the reader
parses anyway and which a browser fetches first. PSTB was considered for
the manifest and not taken: derivations are nested records, not a table,
and PSTB holds at most 32767 rows (16-bit row and level numbers) and sizes only as doubles.

The manifest has no floats because float formatting is where
deterministic JSON writers differ (RFC 8785 needs a whole algorithm for
it). Writing reals as the decimal text that the tool parsed avoids the
problem and records the exact input.

### 9.7 Why the reader checks everything at open but hashes lazily

Structure checks cost microseconds an entry and protect every later
pointer, so they run at open. Hashing a 7 GB film at open would cost
about 2 s cold and is wasted when the session plays one minute of it, so
streamed entries are hashed per chunk as they are read, on the decode
thread. Whole entries are hashed when first used, which is at load, when
their bytes are touched anyway. `YPAK_VERIFY_OPEN` gives a full check
before a session; `YPAK_VERIFY_NONE` exists for a pack checked at
install, and the log says that it was not checked.

### 9.8 Prior art

| Format or tool | What this design takes | What it does not take |
|---|---|---|
| APPNOTE 6.3.10 | zip64 rules, the UTF-8 flag, extra field rules, the 0xD935 entry | encryption, data descriptors, compression methods |
| Android zipalign and apksigner | data alignment by an extra field; 0xD935 | the APK signing block between entries and the directory |
| APK Signature Scheme v4, fs-verity | per-chunk hashes for lazy verification | the Merkle tree depth; signatures |
| Unreal .pak and IoStore | index at the end, per-chunk hashes, compression per block or resource | encryption, its own container |
| Godot .pck | a pack embedded in the executable, found from the end | its own container and MD5 |
| PhysFS | zip as the archive, self-extracting archives found by the end record, path sanitizing | case-insensitive lookup |
| Info-ZIP `zip -A` | absolute offsets in a prefixed archive | |
| miniz, libzip | the central-record-against-local-header consistency check (libzip `ZIP_CHECKCONS`) | the libraries: the reader is ours, bounded and fuzzed |
| Reproducible builds (reproducible-builds.org; `jar --date`) | fixed timestamps, sorted entries, no host data | `SOURCE_DATE_EPOCH`: a pack has no date at all |
| Web Bundles (WICG) | one file of many resources with an index | the format: CBOR, little browser support, no range-first design |

## 10. Open questions, with the defaults in force

The coordinator adopted each recommendation as the default on
2026-10-08. The user accepted questions 1 and 7 on 2026-10-09; each is
one function in the tool.

1. **Calibration in the pack. Decided 2026-10-09 (user): the rig's.** rig_spec 5.2
   lists Calibration as a pack resource; devices_spec 11.2 (2026-10-08)
   says the display calibration is a file on the rig, beside the rig
   profile. Default: the rig's calibration comes from the rig profile; a
   `.yspcal` in a pack is for preview and simulation only and the player
   never applies it on a rig. The tool records `"role": "preview"`
   (`calibration_role()` in `pack/pack_tool.c`). The user's reason: a
   calibration must be independent of the other resources. rig_spec 5.2's
   row says so.
2. **Byte-equal rebuild across compilers. Measured: no difference.**
   ysp/outline.h calls `sin`, `cos`, `atan2`, `tan`, `acos`, `cbrt` and
   `pow`, whose last bit can differ between C runtimes. The tool built with
   MSVC 19.44 (UCRT), MinGW gcc 16.1 (msvcrt), emcc 6.0.10 (musl, run under
   node) and Linux gcc 11.4 (glibc) gave the same bytes on the test corpus
   (5,249,243 bytes, 22 entries: SVG arcs and strokes, a font, glyph runs
   in Latin and Hebrew, textures, WAV conversions) and on a system-font
   corpus (208,223,010 bytes: Segoe UI, Source Han Sans JP (CFF, cubics),
   Microsoft YaHei (a collection), all glyphs; glyph runs in Latin with
   kerning and ligatures, Arabic, Japanese and Chinese; an SVG with
   elliptical arcs, cubic and quadratic smooth curves, skews, rotations,
   round, miter and bevel joins, and a tolerance of 1e-6). 0 bytes differ.
   No CORE-MATH and no change to ysp/outline.h. Not run: macOS (Apple's
   libm), ARM, and an x87 build. The CI should keep the corpus comparison.
3. **Names beyond ASCII. Default: ASCII.** The writer allows the portable
   ASCII set; the reader accepts UTF-8, so a later tool can widen the set
   (NFC, the tool normalizing names) with no format change.
4. **Audio resampling. Default: refuse.** Built: the refusal names the
   rate and the project rate. Built in tool v0.4.0: opt-in resampling with
   libsamplerate 0.2.2 (BSD-2-Clause) at `SRC_SINC_BEST_QUALITY` to 32-bit
   float (`"resample": "sinc_best"`, 4.7). The default stays refuse: the
   author decides that a file is resampled, and the manifest says so.
5. **glslang. Default: optional, not linked.** The shader entry records
   `"validated": false` and flag bit 2 stays 0; ANGLE compiles at load and
   names the body's lines.
6. **Texture compression. Default: raw and QOI.** Built. LZ4 and deflate
   are not built. QOI's ratio on a real stimulus set, measured in phase 3
   (8.2): 45 % of the raw bytes on PsychoPy's 14 demo photographs, built
   no slower than raw. Its decode rate at load is not measured yet.
7. **Font licenses. Decided 2026-10-09 (user).** The tool refuses a face whose
   OS/2 `fsType` says restricted license embedding (bits 0 to 3 equal 2)
   unless the description sets `"private": true`, which the manifest
   records (`font_license_ok()` in `pack/pack_tool.c`). A font with no OS/2
   table passes. Editable (8) and installable (0) embedding pass.
8. **PNG decoder. Default: lodepng.** Pinned at
   `e584a8490db1b76b81bf298c11c09d3b55d9c270` with SHA-256 per file and
   fetched by `tools/vendor_pack.py` (not committed; `.gitignore` ignores
   `third_party/*/`). The layout option does not need it.
9. **Source description syntax. Default: JSON.** Strict: unknown keys,
   duplicate keys and numbers with a fraction are refused by name.
10. **Signed Windows players. Not built.** The reader requires the end
    record at the end of the file, so a player signed after the pack was
    appended does not open its pack. Not measured.
11. **Shader contract version. Built** (ysp/gfx.h v0.10.3,
    `YGFX_SHADER_CONTRACT` = 1, recorded in each SHADER entry). Raised to 2
    in ysp/gfx.h v0.10.5 (the user, 2026-10-09): v0.10.4 binds `stim.tex`
    as `ysp_tex0`, and a body that samples it reads garbage on a contract-1
    player. No pack existed outside the repository.
12. **ffmpeg's bytes on another CPU. Decided 2026-10-20 (user): option
    (b), and libx264 documented** (4.10; built in tool v0.5.0, section
    14). Found 2026-10-20 (9.1) while measuring the thread count:
    with the thread count and the ffmpeg build fixed, the bytes can still
    change with the instruction set that the encoder uses, at one thread
    too. libx264 gave other bytes with its AVX2 code off, and it has no
    switch that makes its output independent of the CPU. mpeg1video and
    mpeg2video gave one set of bytes from the SSE2 level to AVX2 and
    another with every SIMD path off (a proxy for a CPU with other code
    paths, such as ARM); with `-flags +bitexact -dct int -idct simple`
    they gave one set at every level. libx265 gave one set of coded
    pictures at every level, but its info message (an SEI) holds the
    build's OS, compiler and the CPU flags x265 detected, so a pack from
    Windows does not rebuild on Linux unless the author sets `info=0`.
    Options: (a) leave the author's arguments alone and document it; (b,
    chosen) the tool adds `info=0` for libx265 and `-flags +bitexact
    -dct int -idct simple` for mpeg1video and mpeg2video, and records them
    in the command; (c) for libx264, also limit `asm=` to one level that
    every rig has. (c) costs speed and is not proven: x264's AVX-512 code
    (AMD Zen 4 and 5, recent Intel Xeons) and differences between vendors
    could not be measured on this machine, so (c) was not taken: a libx264
    entry rebuilds the same only on a CPU with the same instruction-set
    path, and the hand test of section 14 shows whether the user's AMD rig
    is one.

## 11. Phase 2: what was built

| File | Version | What |
|---|---|---|
| `include/ysp/pack.h` | 0.2.0 | The reader (2.12) |
| `include/ysp/audio.h` | 0.2.1 | `yau_wav_probe()` (docs/audio.md, "The WAV probe") |
| `include/ysp/gfx.h` | 0.10.5 | `YGFX_SHADER_CONTRACT` (docs/gfx.md, "v0.10.3"; 2 from v0.10.5); encoded R8 and RG8 textures, so a TEXTURE entry loads as stored (docs/gfx.md, "v0.10.5") |
| `pack/ysp/pack_tool.h`, `pack/pack_tool.c`, `pack/pack_impl.c` | 0.3.0 | The tool library. 0.1.1 (2026-10-09): the JSON reader and writer moved to `include/ysp/json.h`; every corpus pack is byte-identical to 0.1.0's but for the manifest, which now names ysp/json.h. 0.2.0 (2026-10-09): TEXTURE encoding bytes as `ygfx_texture()` takes them, `primaries` (4.9); the shader contract 2 in SHADER entries. 0.3.0 (2026-10-10): the core build without pack/layout (5.1); the manifest names the layout library only for a pack with glyph runs |
| `include/ysp/json.h` | 0.1.1 | The strict JSON reader and canonical writer (docs/json.md) |
| `pack/ypak.c` | 0.3.0 | The CLI (it prints the tool library's version, and "core build" without pack/layout) |
| `tools/vendor_pack.py` | | Fetches and verifies lodepng |
| `tests/adapt/pack_test.c` | | The reader's test, with its own writer: 29 faults, every bit flip and truncation of a small pack, the four sources, cursors, the arena, a virtual 4.3 GB entry and 65,537 entries (zip64 by size, offset and count), the views |
| `tests/compile/pack.c`, `pack.cpp` | | C11 and C++17 compile checks |
| `tests/fuzz/pack_fuzz.c` | | The libFuzzer target (four targets: memory, incremental, range reader, views) |
| `tests/mutate/pack.toml`, `audio.toml`; `gfx.toml` v10-07 | | Mutations |
| `tests/pack/tool_test.c`, `tests/pack/corpus/` | | The tool end to end: every kind, build twice, verify, rebuild, append, extract, damage, 29 refused descriptions; every corpus texture into `ygfx_texture()` as stored, on the null backend, with and without a calibration |
| `tests/pack/compat.py` | | The six readers (9.1) |
| `examples/pack/bench.c` | | Section 8 (`pack_bench`) |

CMake: ysp/pack.h is in the library list (group `ysp::core`), so its
compile checks and `test_pack` build with every configuration; the tool,
`ypak` and `test_pack_tool` build with `YSP_BUILD_PACK=ON`, with glyph runs
when `YSP_BUILD_LAYOUT=ON` too (5.1).

### Not done

- VIDEO, VIDEO_INDEX, FRAMESEQ and ALPHA in the tool (phase 3); the
  reader reads them. Built in phase 3 but ALPHA (section 13).
- Audio resampling, glslang validation, LZ4, JPEG, ELAN and Praat
  imports, spans in glyph runs, BIDS `n/a` cells. Phase 3 built
  resampling and JPEG, and `.xlsx` tables (section 13).
- Signed players (question 10).
- The bench's cold-SSD number: the phase 1 probe measured it (9.1); the
  bench measures the mapped first touch.
- macOS: nothing was run there.

## 12. A bool open and the message claim (ysp/pack.h v0.2.0)

v0.2.0 applies the repository conventions (README.md, Conventions) to the
reader. The format, the tool and every pack are unchanged.

- **`ypak_open()` returns `bool`.** Open fills a handle from a desc, so it
  returns true or false like every other open. The incremental source
  needs a third answer, "fetch these bytes"; it is true with
  `pk.need_len > 0`, because the handle is valid and waiting, not failed.
  `ypak_feed()` keeps its int: `YPAK_NEED`, 0 or a negative code.
- **`pk.error_code`.** A bool loses the code that the tool's exit status and
  `ypt_verify()` report (`YPAK_ERR_CORRUPT` for a damaged entry at a
  verifying open). The pack keeps the code of its last failure beside the
  message, and `ypak_strerror()` names it.
- **The message claim.** Lookups, `ypak_data()`, `ypak_check()` and
  cursors may run on several threads at once, and each failure writes the
  pack's message. v0.1.0 let two such writes interleave. Now a failure
  takes an atomic claim before it writes (GCC and Clang builtins,
  `_InterlockedExchange` on MSVC) and releases it after; a failure that
  finds the claim taken returns its code and leaves the other text whole.
  No lock is held on the success path, and a failure costs one atomic
  exchange more. README.md names this as the one exception to "the
  message belongs to the last failure": here it is the last failure of
  some thread.

Checked on 2026-10-10 (Windows 11, MSVC 19.44 /W4 /WX as C and C++17,
MinGW-w64 gcc -Werror): `tests/adapt/pack_test.c` adds two threads that
each fail 20,000 lookups with names that share no byte past their
prefix; the message after them is one whole text, the code is
`YPAK_ERR_NOT_FOUND` and the claim is released. Mutants `conv-01` (a
failure that leaves no code) and `conv-02` (a claim never released) are
killed. The tool's test and the fuzz target build against the new open;
`pack/` changed in one line (`open_pack()` reads `pk.error_code`).

## 13. Phase 3: what was built (tool v0.4.0, 2026-10-13)

Phase 3 is the tool's half of the kinds that phase 2 left to it, in the
order of section 11's list, and the source formats that PsychoPy's demos
use (rig_spec 15.4). The reader, `include/ysp/pack.h` v0.2.0, did not
change: it already read every kind.

| File | Version | What |
|---|---|---|
| `pack/ysp/pack_tool.h`, `pack/pack_tool.c` | 0.4.0 | VIDEO with its VIDEO_INDEX, the ffmpeg step and its rebuild (4.10, 5.3); FRAMESEQ (4.10); TABLE from `.xlsx` (4.1); TEXTURE from JPEG, ICC profiles that are sRGB's (4.9); audio resampling (4.7); `ypt_rebuild_ex()` with `ypt_options.ffmpeg` |
| `pack/import.c` | new | The readers of other formats: `ypt_xlsx_read()`, `ypt_jpeg_read()`, `ypt_icc_srgb()` |
| `pack/stb_image.c`, `pack/pack_impl.c` | | stb_image compiled JPEG only; the ysp/video.h implementation in the tool |
| `pack/ypak.c` | 0.4.0 | `--ffmpeg PATH` for `build` and `rebuild` |
| `tools/vendor_pack.py` | | Fetches and checks stb_image, pl_mpeg and libsamplerate beside lodepng |
| `tests/adapt/import_test.c`, `tests/adapt/import_gen.h` | | The readers on their own; writers of ICC profiles, JPEG files and workbooks that the tool test shares |
| `tests/pack/tool_test.c`, `tests/pack/corpus.sha256` | | New kinds and sources in the pinned corpus (30 entries), 34 new refusals, video through ffmpeg, the resampled sine |
| `tests/fuzz/import_fuzz.c` | | libFuzzer: two workbook targets, JPEG, ICC |
| `tests/mutate/import.toml`, `tests/mutate/mutate.py` | | 38 mutants of `pack/import.c`; the runner can now mutate a `pack/*.c` file |
| `tests/pack/throughput.py` | | Section 8.2 |

Third-party code added, each fetched at a commit and checked by SHA-256
(`tools/vendor_pack.py`), none committed:

| Code | License | Pin | Use |
|---|---|---|---|
| stb_image.h 2.30 | public domain or MIT | nothings/stb `2c980bb`, `stb_image.h` SHA-256 `594c2fe3...` | JPEG |
| pl_mpeg.h | MIT | phoboslab/pl_mpeg `c871f2b`, SHA-256 `3a8cb30c...` (the top-level `YSP_FETCH_PL_MPEG` pin) | MPEG-1 indexes |
| libsamplerate 0.2.2 | BSD-2-Clause | libsndfile/libsamplerate `c96f5e3` (tag 0.2.2), 10 files, `src_sinc.c` SHA-256 `8e59137d...` | resampling |

The manifest names each (`tool.libraries`), with `,tree-differs` when
the fetched file is not the pinned one.

### Decided (the user, 2026-10-14)

- **ICC profiles.** A profile is accepted in PNG and JPEG only once it is
  identified as sRGB's, and refused otherwise (4.9). It is identified by
  its content, never by its name: an RGB matrix-and-curves profile whose
  three colorants (rXYZ, gXYZ, bXYZ) are each within 0.002 of sRGB's D50
  values and whose three curves (rTRC, gTRC, bTRC, a table or a
  parametric curve) are each within half a code of 255 of sRGB's curve at
  all 256 codes. v0.3.0 refused every `iCCP`.
- **EXIF orientation.** Anything but 1 is refused, not applied: browsers
  apply it and PIL does not, so either choice would surprise someone.
- **Workbooks.** Dates and times are refused; a value in a column without
  a name is refused (PsychoPy drops the column); a column of text cells
  stays text. Booleans are `TRUE` and `FALSE` as text.
- **ffmpeg.** Changed from the proposal: the tool encodes on one thread
  itself (`-threads 1`; for libx265 also `pools=1:frame-threads=1`), so a
  pack rebuilds byte for byte on any machine without the author asking,
  and the command records it (4.10). The author's thread settings are
  refused. The cost is measured in 9.1, phase 3. Replaced on 2026-10-20
  by a recorded thread count (section 14).
- **MP4 indexes** are made on Windows only (Media Foundation), for now;
  MPEG-1 on every platform. A rebuild elsewhere takes the recorded index.

Proposed in phase 3 and not reviewed:
- **Resampling** is opt-in (`"resample": "sinc_best"`); refusal stays the
  default (open question 4).
- **ALPHA** stays unbuilt (4.11).

### Left

- ALPHA, until an experiment needs it.
- glslang validation of shaders (open question 5) and LZ4 or deflate
  textures (open question 6): not built, the defaults stand.
- MP4 indexes off Windows: ysp/video.h has no FFmpeg or AVFoundation
  backend.
- A video source is read whole into memory, hashed and copied; a two-hour
  film needs its size in memory. Streaming the copy and the hashes is not
  built.
- The ffmpeg step takes one source file; an image sequence
  (`frames/%04d.png`) as ffmpeg's input is not taken, because the manifest
  hashes one source file.
- MP3 sources, ELAN and Praat imports, BIDS `n/a` cells, spans in glyph
  runs.
- emcc (the designer's build) and macOS: phase 3 was not run there. The
  ffmpeg step cannot run in a browser.

## 14. Tool v0.5.0: the encode's thread count (2026-10-20)

Decided by the user on 2026-10-20 (option c of three: one thread, a
fixed count, or a recorded count): the tool picks the ffmpeg step's
thread count, passes it, records it, and `ypak rebuild` runs ffmpeg with
the recorded count. The decision waited on one measurement: does a fixed
count give the same bytes on any machine? It does, for every encoder
measured (9.1, 2026-10-20). The same measurement found that the bytes
can depend on the CPU's instruction set at any count, one included.
Decided by the user the same day (open question 12): the tool adds
`info=0` for libx265 and `-flags +bitexact -dct int -idct simple` for
mpeg1video and mpeg2video, refuses the author's contrary settings, and
records the flags in the command; libx264, which has no such flag,
rebuilds the same only on a CPU with the same instruction-set path, and
4.10 says so.

| File | Version | What |
|---|---|---|
| `pack/ysp/pack_tool.h`, `pack/pack_tool.c` | 0.5.0 | `threads` (4.10): `-threads N`, libx265's `pools=N:frame-threads=F`, `"threads": N` in the derivation, which a rebuild takes back; `info=0` for libx265 and `-flags +bitexact -dct int -idct simple` for mpeg1video and mpeg2video (merged into the author's `-flags`), the author's `info`, `-dct`, `-idct` and `-bitexact` refused; ffmpeg's version line refused past 255 bytes instead of cut |
| `pack/ypak.c` | 0.5.0 | unchanged but for the version |
| `tests/pack/tool_test.c` | | 4 refusals of `threads` and 5 of the flags (x265 `info=1` and a bare `info`, `-dct`, `-idct:v` with the default encoder of `.mpg`, `-flags +cgop-bitexact`); the author's 3 for MPEG-1 and the machine's count for H.264 in the command and the derivation; `+bitexact` merged into the author's `-flags +psnr` and the DCTs for MPEG-1; libx265's pool, frame threads and `info=0` for `"threads": 4`; the rebuild with the recorded 3 |
| `tests/pack/rebuild_threads/` | | The hand test below: three descriptions and `run.sh` |

Every entry of the test corpus but the manifest is byte-identical to
v0.4.0's; a video entry made by ffmpeg differs (its command and count).

### Measured

The bytes: 9.1, 2026-10-20. The cost, 2026-10-20, the i7-1360P on AC,
under the measurement lock (load 14 % at the start), ffmpeg
n9.0.2-28-g669f5ab083 (Windows), the encode alone as the tool runs it
(`-threads N`; libx265 `pools=N:frame-threads=F`), median of 3 rounds
(min to max), the counts interleaved in each round:

| Source | Encoder | 1 thread | 8 threads | 16 threads (this machine's default) | 1 thread / 16 |
|---|---|---|---|---|---|
| PsychoPy's demo movie, 640 x 480, 173 frames | libx264 medium, crf 18 | 1.17 s (1.14 to 1.47) | 0.51 s (0.51 to 0.63) | 0.48 s (0.47 to 0.58) | 2.4 |
| the same | libx265 medium, crf 20 | 2.31 s (2.18 to 3.10) | 1.17 s (1.05 to 1.45) | 1.32 s (1.09 to 1.43) | 1.7 |
| the same | mpeg1video, q 3 | 0.25 s (0.24 to 0.27) | 0.19 s (0.17 to 0.19) | 0.21 s (0.17 to 0.21) | 1.2 |
| the same | mpeg2video, q 3 | 0.24 s (0.24 to 0.27) | 0.19 s (0.18 to 0.21) | 0.20 s (0.18 to 0.20) | 1.2 |
| testsrc2, 1920 x 1080, 300 frames | libx264 medium, crf 18 | 17.39 s (12.02 to 25.04) | 4.51 s (4.17 to 8.13) | 4.50 s (3.53 to 5.07) | 3.9 |
| the same | libx265 medium, crf 20 | 28.89 s (28.08 to 31.74) | 9.55 s (9.38 to 20.71) | 9.41 s (8.63 to 23.40) | 3.1 |
| the same | mpeg1video, q 3 | 1.21 s (1.18 to 1.59) | 0.80 s (0.78 to 0.85) | 0.68 s (0.61 to 0.77) | 1.8 |
| the same | mpeg2video, q 3 | 1.18 s (1.16 to 1.20) | 0.66 s (0.64 to 0.67) | 0.59 s (0.56 to 0.60) | 2.0 |
| the hand test's `x264.json`, the whole `ypak build` (encode, index, hashes, write) | libx264 | 1.60 s (1.50 to 1.62) | | 0.94 s (0.89 to 1.03) | 1.7 |

The forced flags of mpeg1video and mpeg2video (`-flags +bitexact -dct
int -idct simple`) in the same session, against the default flags:

| Source | Encoder | 1 thread | 16 threads |
|---|---|---|---|
| the movie | mpeg1video | 0.32 s against 0.25 s (1.3 times) | 0.23 s against 0.21 s (1.1 times) |
| the movie | mpeg2video | 0.38 s against 0.24 s (1.6 times) | 0.24 s against 0.20 s (1.2 times) |
| 1080p | mpeg1video | 1.59 s against 1.21 s (1.3 times) | 0.74 s against 0.68 s (1.1 times) |
| 1080p | mpeg2video | 1.77 s against 1.18 s (1.5 times) | 0.98 s against 0.59 s (1.7 times) |

At most 0.4 s on 10 s of 1080p film here; `info=0` costs nothing (it
drops one SEI).

The ranges are wide at 1080p (libx264 on one thread 12 to 25 s; one round
of libx265 on 8 and on 16 threads 2.2 times its median): this laptop
changes its clock with temperature, and the medians are the numbers to
use. The 2026-10-14 table (9.1) measured another ffmpeg build and
ffmpeg's default threads (libx264 picks 1.5 times the logical processors
itself), so its ratios of 10.2 and 5.5 are not this table's 3.9 and 3.1.
At 1080p, 16 threads cut the encode to a quarter (libx264) or a third
(libx265); 8 threads did as well as 16 on this machine of 4 P-cores.

Tests, 2026-10-20: MSVC 19.44 `/W4 /WX`, core build, 265 checks, 0
failed (MPEG-1, H.264 and HEVC through ffmpeg N-122953); Linux gcc 11.4
(WSL2), Release and ASan with UBSan, core build, 260 checks, 0 failed
each (MPEG-1 through ffmpeg 4.4.2); MinGW gcc 16.1 compiles
`pack/pack_tool.c` with CI's warnings and `-Werror`.

Mutants by hand (the runner cannot build the tool), MSVC 19.44,
`test_pack_tool`, 17 of 17 killed: no `-threads N` (3 checks); the count
not recorded (4); the author's count ignored (2); 1 as the default
instead of the machine's count (1); a rebuild that drops the recorded
count (2); libx265's pool as a second `-x265-params` (1: B-frames return,
the index refuses them); libx265's frame threads equal to N (1); no range
check (2); `threads` without `ffmpeg` taken (1); the author's thread
and flag settings taken (8); `info=0` not added (1); the MPEG flags not
added (1); `+bitexact` as a second `-flags` (1: it replaces the author's);
the author's `info` taken (2); the author's `-dct` or `-idct` taken (3);
no MPEG default for `.mpg` without `-c:v` (1); a bare `info` not seen as a
setting (1).

The hand test below, run with WSL2 standing in for the second machine
(the Linux build of ypak at gcc 11.4, 8 logical processors, the packs'
recorded 16): `identical` for all three packs.

### Hand test: a pack from Windows rebuilt on the Linux rig

The question it answers: does a pack built on the Windows laptop with 16
threads rebuild byte for byte on the AMD rig, with the same ffmpeg
version? It also shows whether libx264 takes the same instruction-set
path on that rig's CPU (4.10).

1. Get one BtbN FFmpeg-Builds release (github.com/BtbN/FFmpeg-Builds,
   one `autobuild-...` tag): its `win64-gpl` build for Windows and its
   `linux64-gpl` build of the same version for the rig. The two must
   print the same first line of `ffmpeg -version`. Measured with
   `ffmpeg-n9.0.2-28-g669f5ab083-{win64,linux64}-gpl-9.0` from
   `autobuild-2026-10-19-13-06`.
2. On Windows, build `ypak` (`YSP_BUILD_PACK=ON`), then in Git Bash:

   ```sh
   sh tests/pack/rebuild_threads/run.sh build build/pk/pack/Release/ypak.exe \
       /path/to/win64/bin/ffmpeg.exe \
       /path/to/psychopy/demos/coder/stimuli/jwpIntro.mp4 /c/tmp/threads-pack
   ```

   It writes `mpeg1.ysppak`, `x264.ysppak` and `x265.ysppak` (each
   records `"threads": 16`), their descriptions and the movie.
3. Copy `/c/tmp/threads-pack` to the rig.
4. On the rig, fetch the sources (`tools/vendor_pack.py --write`), build
   `ypak` (`cmake -S . -B build/pk -DYSP_BUILD_PACK=ON
   -DCMAKE_BUILD_TYPE=Release`, then `cmake --build build/pk --target
   ypak`), and run:

   ```sh
   sh tests/pack/rebuild_threads/run.sh rebuild build/pk/pack/ypak \
       /path/to/linux64/bin/ffmpeg /path/to/threads-pack
   ```

5. Expected: `identical` three times, and exit 0. The MP4 indexes come
   from the packs (a `note:` line says so): there is no MP4 index off
   Windows. `mpeg1` and `x265` must be `identical` on any x86-64 CPU
   (the tool's flags, 4.10). `x264` is `identical` only when x264 takes
   the same instruction-set path as on the laptop (AVX2, no AVX-512); if
   it differs and the script prints `AVX-512: yes`, that is the libx264
   limit of 4.10, not the thread count. Record the outcome and the CPU
   line in 9.1.

### Left

- The hand test on the AMD rig.
- ARM (macOS, a Raspberry Pi): the flags were measured with every x86
  SIMD path off as the proxy, not on an ARM CPU.
