# ysp/json.h

Status: v0.2.0, 2026-10-21 (canonical numbers); v0.1.0, 2026-10-09.
The header's manual is the reference (THE READER, VALUES, THE BUILDER,
THE WRITER, CANONICAL NUMBERS, THE ARENA). This page records why the
header exists, the decisions, and the results.

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
   `yjs_push()`, `yjs_new_int()`, `yjs_new_fixed(ns, 9)` for a duration,
   `yjs_new_double(x)` for any other real, `yjs_new_stringz()`.
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
| Other numbers in the canonical form | v0.1: written as their text. v0.2.0: their exact value in one spelling (Canonical numbers, below) | v0.1's profile and manifest built every number from integers; the experiment definition (experiment_format.md D2, gap G7) has reals written by hand |

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

## Canonical numbers (v0.2.0)

The experiment ID is the SHA-256 of the definition's canonical bytes
(experiment_format.md D2). v0.1 wrote a number's source text, so `0.5`
and `0.50` gave two IDs for one experiment (gap G7). v0.2.0 gives each
value one spelling.

### Decisions

| Question | Decision | Why |
|---|---|---|
| What the writer writes for a parsed number | Its exact decimal value, respelled; never a value rounded through a double | Every accessor reads the canonical text as it read the source: `yjs_double()` the same bits, `yjs_fixed()` the same int64 at every scale, `yjs_int64()` the same integer. A round through a double would change a duration of 17 or more digits (`yjs_fixed()` reads up to 19) and could not keep `yjs_new_fixed()` text. Respelling is text work only, so no platform's floating point is involved |
| `1.0`, `1e2` | Written `1` and `100`: integer text | One value, one spelling. `yjs_int64()` refused the source text and reads the canonical text; a loader that checks integers checks the canonical bytes, which are what the pack holds |
| Plain or exponent | Plain when the first digit's exponent E is from -18 to 20; else `d.ddde+E` or `d.ddde-E` | -18 keeps every `yjs_new_fixed()` text (at most 18 decimals) and 20 keeps every int64 integer, so no byte that was canonical in v0.1 changes. 21 is JavaScript's switch (`1e+21`). A double's text is at most 37 bytes. D2 asked for no exponent; a bound is needed, because without one `5e-324` is 326 bytes and `1e999999999` a gigabyte |
| The exponent's spelling | `e`, then `+` or `-`, no leading zeros | JavaScript's `String(x)`, so a web player can compare text with no rule of its own outside 10^-18 to 10^-6 |
| Zero | `0`, also for `-0`, `-0.0`, `0e5`, and `yjs_format_double(-0.0)` | D2. The sign of a zero is lost; nothing in a definition depends on it |
| A double's digits | The shortest decimal that reads back to the double; of those, the nearest; an exact tie to the even digit | Python's `repr`, V8's `String(x)` and Ryu all give these digits (measured below), so three implementations agree |
| The algorithm | Ryu (Adams 2018), small-table variant, integer arithmetic only | No printf, no strtod, no FPU mode: the digits cannot depend on the compiler or the C library. The full Ryu tables are 10.7 KB; the small ones 780 bytes (13 + 13 multipliers of 125 bits, 26 powers of 5, 2-bit corrections), at one extra 64 x 128-bit multiply. Grisu needs a bignum fallback for 0.5 % of doubles, two algorithms to test. A bignum-only Dragon4 is simple but costs microseconds per number |
| The exponent limit | The reader refuses a nonzero number whose first digit is past 10^+-999999999 | RFC 8259 section 9 lets a reader limit the range of numbers. With the limit, every canonical exponent has at most 9 digits and reads back; without it, `1234567890e999999999` respells to an exponent the reader then refuses (the fuzz target found this) |

### How D2 is met

D2 asks that numbers stay JSON numbers in a canonical decimal form. The
writer spells every number canonically, so the canonical bytes of a
definition depend on its values, not on how a person typed them: `0.5`,
`0.50`, `5e-1` and `0.5E0` give one ID. A duration written through
`yjs_new_fixed(ns, 9)` and a real written through `yjs_new_double(x)`
are canonical too, and stay so through any number of parses and writes.
One case stays open: two texts of one double that differ past its 17th
digit (`0.1` and `0.1000000000000000055`) are different exact values and
give different IDs, though the player reads one double. A loader that
writes each real back through `yjs_double()` and `yjs_new_double()`
makes them one ID; that choice belongs to the loader
(experiment_format.md D2).

### Results

On 2026-10-21, Windows 11, MSVC 19.44 (x64, /O2), MinGW-w64 gcc 16.1
(-O2), WSL2 Ubuntu 22.04 gcc 11.4 (-O2; -O2 -march=native
-ffp-contract=fast; -O1 ASan with UBSan), emcc with node 24.19 (wasm32).

| Check | Result |
|---|---|
| The reference | Python's `repr` (David Gay's correctly rounded shortest conversion) respelled by `tests/adapt/json_dtoa_ref.py`, which shares no code with the header |
| 10,000,000 doubles in 8 classes (random bits, subnormals, decimals of 1 to 17 digits and their neighbors 1 ulp away, powers of two and their neighbors, integers below 2^53, 15 to 17 digit decimals) | FNV-1a digest of the texts equal to Python's on MSVC, MinGW, WSL2 gcc, gcc with fused multiply-adds, and wasm: `0xde6a84779f6a9401`. Every text reads back to its double through `yjs_parse_double()`, and no decimal one digit shorter does |
| 2,000,000 of them against V8 | `String(x)` respelled gives the same text for all; `String(x)` itself is the canonical text for 98.6 % (the rest are below 10^-6 or -0) |
| 70 edge doubles (0, -0, the smallest subnormal, the largest subnormal and double, powers of 2 and 10, 2^53 and its neighbors, the plain/exponent boundaries, Ryu's regression list, 2.98023223876953125e-8, an exact tie) | Each equal to Python's text |
| Every multiplier of the small tables (326 + 291) | FNV-1a checksum equal to the exact values' (Python integers) |
| 5,000,000 random number texts (runs of zeros, exponents with leading zeros, up to 10^9) | Digest equal to Python's on every compiler above; the canonical text reads the same through `yjs_parse_double()` (bits and code) and `yjs_parse_fixed()` at 0, 3, 9 and 18 decimals; it is its own canonical text |
| v0.1 canonical bytes | 100,000 random `yjs_new_int()` and `yjs_new_fixed()` values (0 to 18 decimals) written byte for byte as their v0.1 text; `tests/adapt/rigfile_test.c` (golden profiles) passes unchanged |
| `/fp:fast` (MSVC) | Not supported: the reader's own checks fail (a signed zero, values near 10^300, `HUGE_VAL` folded to 10^300); the formatter uses no floating point |
| Fuzzing (`tests/fuzz/json_fuzz.c`) | Targets 0 and 1 also check that every number of the canonical text reads the same through each accessor; target 3 formats the input's first 8 bytes as a double and checks the round trip. MSVC libFuzzer with ASan, the 17 seeds: one finding after 68,711 inputs (`1234567890e999999999` respells to a 10-digit exponent that the reader refused; the reader now limits the first digit's exponent), then 3,405,771 inputs in 1201 s with no finding |
| Mutants (`tests/mutate/json.toml`) | 40 in all, 39 killed. 12 new (j-29 to j-40); j-37 survives as an equivalent mutant (its note says why; 300,000 targeted doubles give Python's text). j-24 (skip the int64 path) became equivalent, because the respelling writes the same bytes; it now drops the path's minus sign, and is killed |

Costs (`examples/rigfile/bench.c`, Iris Xe laptop on AC, under the measurement lock (load 0 % at its start), 21 rounds of 4096 calls, medians; MSVC 19.44 /O2 and MinGW-w64 gcc 16.1 -O2):

| Cost per number | MSVC | MinGW |
|---|---|---|
| `yjs_format_double()`, random bits (mostly 17 digits) | 68.7 ns | 68.7 ns |
| `yjs_format_double()`, k/1000 (1 to 3 digits) | 60.4 ns | 61.5 ns |
| `snprintf("%.17g")` beside it, random bits | 813 ns | 1615 ns |
| `snprintf("%.17g")`, k/1000 | 229 ns | 689 ns |
| `yjs_write()` of a parsed real (`0.2500`), respelled | 22.1 ns | 29.9 ns |

v0.1.1 against v0.2.0, MinGW, the same flags, two alternating runs in
one timing session (load 20 % at its start, so the spread is wide): the
rig profile's `yrig_write()` 8.4 and 8.5 us against 12.0 and 9.0 us (its
durations are now respelled, about 20 ns each); the 1 MB manifest-like
document, integers and strings only, writes at 631 and 599 MB/s against
742 and 523 MB/s, no change outside the spread.

A definition with a thousand reals spends at most 0.1 ms (70 us to format them, 30 us to respell them) on them per write.

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
- No run on macOS arm64 of v0.2.0 here; CI runs `json_test.c` there,
  and its digests must equal the constants in the test.
- The fuzz target is not in CI, as the other fuzz targets.
