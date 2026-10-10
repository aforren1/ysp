# ysp/json.h

Status: v0.1.0, 2026-10-09. The header's manual is the reference (THE
READER, VALUES, THE BUILDER, THE WRITER, THE ARENA). This page records
why the header exists, the decisions, and the results.

## Why a header

The player reads JSON at run time: the rig profile now (`ysp/rigfile.h`,
docs/devices_spec.md 11.2), the experiment definition later if its
format is JSON (rig_spec 6, not decided). The pack tool already had a
strict reader and a canonical writer in `pack/pack_tool.c`. Two readers
of one language disagree at the edges (a duplicate key, a lone
surrogate, `-0`, a number past int64), and the edges are where a hostile
or hand-edited file goes. So the pack tool's reader became this header,
and the pack tool reads through it: one parser in the repository.

## How-to: read a file's values

1. Make an arena: `yjs_arena_init_heap(&a, 0)` (blocks from the heap, at
   most 256 MiB), or `yjs_arena_init(&a, buf, cap)` (the caller's memory,
   no heap).
2. `yjs_parse(&a, text, n, NULL, &root, &err)`. On a refusal, print
   `err.line`, `err.column` and `err.msg`.
3. Read values: `yjs_get(obj, "key")`, `yjs_at(arr, i)`,
   `yjs_string()`, `yjs_int64()`, `yjs_fixed(v, 9, &ns)` for seconds as
   nanoseconds, `yjs_double()`. Each accessor returns `YJS_ERR_TYPE` for a
   missing value or another type, so one check covers both.
4. `yjs_arena_free(&a)` frees every value at once.

## How-to: write canonical bytes

1. Build values in an arena: `yjs_new(&a, YJS_OBJECT)`, `yjs_set()`,
   `yjs_push()`, `yjs_new_int()`, `yjs_new_fixed(ns, 9)`,
   `yjs_new_stringz()`.
2. `yjs_write_mem(root, YJS_WRITE_PRETTY, buf, cap)` gives the length;
   call it with `buf` NULL first to size the buffer. `yjs_write()` gives
   the text to a callback instead.

## Decisions

| Question | Decision | Why |
|---|---|---|
| A header or the pack tool's code | A header, `ysp/json.h`, prefix `yjs_` (`YJS_`); no clash in the README's Prefix column | The player needs it at run time, and the headers are the only code the player links that is not the player's |
| Numbers | Kept as their text; flags say integer text and int64 range; `yjs_int64()`, `yjs_fixed()`, `yjs_double()` | A reader that converts at once must choose a type for the caller. Durations in seconds read exactly as nanoseconds through `yjs_fixed()`, with no binary fraction in between |
| The double conversion | ysp/table.h's (Clinger's fast path, then exact big integers), copied | Correctly rounded and free of the locale; tested there on 10 million strings. A copy keeps the header free of dependencies |
| Objects | Sorted by key when they close; the builder inserts in order | One sort finds duplicates in O(n log n) (a hostile object of 10^6 keys costs no quadratic scan), lookups are binary searches, and the writer needs no sort and no allocation. The order of the source text is not needed by any reader here; positions stay for messages |
| Values | 32 bytes each, pointers between them, all in one arena | One free; no per-value allocation; pointers let one value sit in two containers, as the pack tool's manifest builder needs |
| The scratch stack of the parser | The top of a fixed arena, or one heap buffer | A fixed arena then needs no heap at all |
| `\u0000` | Refused | Every string is then a C string; the pack tool refused it already |
| A byte order mark | Skipped at the start | RFC 8259 lets a reader ignore it; Windows editors write it |
| The canonical form | The pack manifest's (docs/pack.md 7): keys in bytewise order, two spaces, one member per line, an array of scalars on one line, `\u00XX` lowercase for control bytes, int64 integers in plain decimal | It existed, and the corpus of the pack tool pins it |
| Other numbers in the canonical form | Written as their text | A shortest round-trip form for every double needs Ryu or Grisu; the profile and the manifest build their numbers from integers (`yjs_new_fixed()`, `yjs_new_int()`), so their text is canonical already |

## Results

On 2026-10-09, MSVC 19.44 and MinGW-w64 gcc 16.1 on Windows 11, gcc 11.4
on WSL2.

| Check | Result |
|---|---|
| `tests/adapt/json_test.c`, 470 checks | Pass on MSVC (Release), MinGW (Release and -O1), WSL2 gcc (-O2, and ASan with UBSan) |
| The grammar | 14 accepted documents written back as expected; 62 refused, each with its line, column and message (comments, trailing commas, leading zeros, NaN, lone and reversed surrogates, overlong UTF-8, `\u0000`, duplicates in small and 5000-key objects) |
| Doubles | 200,042 strings, equal to `ytb_parse_number()` and `strtod()` bit for bit on each compiler, 0 disagreements |
| Fixed point | Ties to even, sticky digits, the int64 ends, exponents of 10^8; 20,000 random int64 values through `yjs_new_fixed()` and back |
| Damage | 40,000 random edits of a profile-like document: every refusal has a line, a column and a message; the 6335 that are still JSON write canonical text that parses to the same bytes (the same count on every compiler) |
| Arena | 48 bytes per number in an array of 20,000; the smallest fixed buffer for a document is found and 8 bytes less is `YJS_ERR_NOMEM`; an odd buffer address; the heap limit holds |
| The pack tool on it | `tests/pack/tool_test.c` passes; all 8 packs of the test are byte-identical to the old parser's (SHA-256 of each file compared, the manifest included), then the tool (0.1.1) added `ysp/json.h` to the manifest's libraries; `tests/pack/corpus.sha256` unchanged |
| Fuzzing (`tests/fuzz/json_fuzz.c`) | MSVC libFuzzer with ASan, all four targets (JSON in a fixed arena, in a heap arena with a depth from the input, the rig profile, the latency file and the number parsers): 14,150,487 inputs in 1501 s, no crash and no broken round trip; the 2166 corpus files replayed on WSL2 gcc under ASan and UBSan with no report |
| Mutants (`tests/mutate/json.toml`) | 28 of 28 caught on the first run |

Costs (`examples/rigfile/bench.c`, Iris Xe laptop on AC, under the measurement lock (load 9 %), 21 rounds, MSVC 19.44 /O2 and MinGW-w64 gcc 16.1 -O2; a 3453-byte profile with 4 roles and every field):

| Cost | MSVC | MinGW |
|---|---|---|
| `yjs_parse()` of the profile into a fixed arena | 4.9 us | 4.4 us |
| `yrig_parse()` of the profile: the parse, every field check, the struct | 23.3 us | 17.7 us |
| `yrig_write()`: the canonical bytes | 7.0 us | 6.6 us |
| `yrig_hash()`: the bytes, the read-back check, SHA-256 | 42.6 us | 43.1 us |
| `yjs_parse()` of a 1 MB manifest-like document | 351 MB/s | 604 MB/s |
| `yjs_write()` of it, pretty | 803 MB/s | 1144 MB/s |

Medians; the ranges are in `C:\tmp\psy-work\rig\bench_*.txt`. The 1 MB
document takes 3.7 MB of arena (3.6 times its text). A profile is read
once per session, so its cost is not a frame-budget question; the numbers
say a profile check costs less than one display frame by three orders of
magnitude.

## Conventions check (v0.1.1)

v0.1.1 checks this header against the repository conventions (README.md,
Conventions) and changes no code. The arena is a plain struct that grows
into heap memory, so it has `yjs_arena_init()` and `yjs_arena_free()`, not
open and close. `yjs_parse()` has no handle: it writes its message into the
caller's `yjs_error`, which is the caller-buffer rule with a line and a
column added. Every call that can fail returns 0 or a negative
`YJS_ERR_*`; `yjs_number_ok()` and `yjs_utf8_ok()` are predicates and
return `bool`. The one time-shaped call, `yjs_fixed(v, 9, &ns)`, already
reads seconds as `int64_t` nanoseconds. The USAGE block has no designated
initializers. Only the CHANGELOG moved, to the end of the top comment.
Checked on 2026-10-10 with MSVC 19.44 /W4 /WX and MinGW-w64 gcc -Werror:
`tests/adapt/json_test.c` passes unchanged.

## Not done

- No run on macOS or a big-endian host.
- No streaming (SAX) reader: a document is parsed whole. The largest one
  in view, a pack manifest, is a few MB.
- No shortest round-trip form for a double in the writer (see
  Decisions).
- The fuzz target is not in CI, as the other fuzz targets.
