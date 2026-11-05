# ysp/table.h design notes

Status: v0.3.0, 2026-10-21: 256 columns (see "256 columns (v0.3.0)").
v0.1.0, 2026-10-07. Written with ysp/trials.h v0.2.0, which takes
its conditions from a table (`desc.table`). The coordinator approved the
design on 2026-10-07 and asked for the table code to be its own header, so
the pack tool and ysp/timeline.h can read tables through it later. The
header's manual says what it does; this page records why, and the
measurements.

## Why a table header

The user asked for fixed trial lists and conditions files from CSV, with
hard bounds, typed columns, errors that name the row and column, and a
number syntax that the locale cannot change. The pack (rig_spec 5.2)
already planned a "Table" resource of typed binary columns, read by
ysp/trials.h and ysp/timeline.h. One in-memory form serves both: the CSV
parser writes the same block that the pack stores, and a view reads either
in place.

## Decisions

| Question | Decision | Why |
|---|---|---|
| A header of its own | Yes (coordinator, 2026-10-07) | Two readers (ysp/trials.h now, ysp/timeline.h and the pack tool later). ysp/trials.h requires it, as ysp/gfx.h requires ysp/color.h. |
| One block for CSV and pack | Yes: position-independent, little-endian, 8-byte aligned sections, offsets not pointers | A pack entry is mapped and viewed with no copy; a parse result can be written to a pack unchanged. |
| Every column categorical | Yes: levels are the distinct values in order of first appearance; a cell stores a 16-bit level | Constraints, tallies and the rules text name any column; 2 bytes per cell. |
| Numeric levels | Equal values are one level; the level keeps its first text | "0.5" and "0.50" are one condition; data files show numbers as written. |
| Type inference | Declared types win; else INTEGER, NUMBER or STRING by the cells, and an out-of-range or too-long number still counts as a number | A typo in one cell is reported by row and column instead of turning the column into text. |
| Number parser | Own, correctly rounded, no locale | `strtod` follows LC_NUMERIC. |
| File I/O | Only with `YTB_STDIO` | Pure computation by default, as the user asked. |
| Integers | int32 (coordinator) | Onsets in event files are numbers, not integers. |

## The number parser

The syntax is stated in the manual: decimal digits, an optional point and
exponent, at most 19 significant digits, no `inf`, `nan`, hex, spaces or
separators. Three exact paths, the first that applies:

1. Clinger's fast path: the digits fit 2^53 and the power of ten is at
   most 22. One exact multiply or divide by an exact power of ten.
2. A two-word path for exponents of magnitude at most 27, where 5^|E| fits
   63 bits: a 128-bit product, or a 128-by-64-bit division for a quotient
   between 2^55 and 2^57, with the remainder as the sticky bit. The
   compiler's 128-bit type where it has one, else the same results from
   32-bit pieces (MSVC).
3. Big integers for the rest: M x 5^E rounded to 53 bits, or a restoring
   division of M x 2^s by 5^-E. Subnormals are rounded at their own bit.

The second path was added by measurement. With paths 1 and 3 only, a
column of pandas-style 17-digit numbers parsed at 17 MB/s, because every
such number needs path 3. With path 2 the same file parses at the rates
in "Cost" below.

Beyond the limits: a 20th significant digit is `YTB_NUM_DIGITS`
(trailing zeros after the point do not count); a value whose magnitude
rounds above the largest double, or a nonzero value that rounds to 0, is
`YTB_NUM_RANGE`. In a CSV both are errors that name the cell; the manual
says how to keep a code column as text.

## Verification

All on Windows 11, MinGW-w64 gcc 16.1 (msvcrt) and MSVC 19.44, as C11,
C99 (gcc) and C++17, warnings as errors; and on Linux (WSL2, gcc 11.4)
under AddressSanitizer and UndefinedBehaviorSanitizer.

### Numbers against strtod

`tests/adapt/table_test.c` compares `ytb_parse_number()` with
`strtod()` in the C locale, bit for bit, after checking the syntax
independently: 84 fixed strings (halfway points around 2^53, the
subnormal and normal boundaries, the largest double and its neighbours,
the 19-digit limit, syntax errors) and a random corpus built to hit the
hard cases (exact halfway points (2a + 1) 2^k and their neighbours,
`%.13e` to `%.17e` round trips of random doubles and subnormals,
neighbours of the range ends, long zero runs). The default run uses
300,000 random strings.

| Run | Strings | Disagreements |
|---|---|---|
| gcc 16.1, MinGW (its strtod is mingw-w64's) | 10,000,084 | 0 |
| MSVC 19.44 (UCRT strtod) | 10,000,084 | 0 |
| Linux gcc 11.4 (glibc), sanitizers | 200,084 | 0 |
| Python binding against `float()` (CPython 3.14) | 5,000 | 0 |

The same check runs after `setlocale(LC_ALL, "German")`, where `strtod`
reads "0,5" as 0.5: the parser still reads "0.5" as 0.5 and refuses
"0,5". Linux had no decimal-comma locale installed, so the locale check
ran on Windows only.

### CSV

One check per dialect rule and bound of the manual, each error checked
for its line, row and column; 64 columns, 32767 rows, 4096-byte fields and
63-byte names at and past the limit. The Python binding's test compares
the parser with Python's `csv` module on 40 random files with random
quoting, embedded delimiters and line breaks, CRLF and LF, a BOM, and
non-ASCII text: every cell equal.

### The block

The same input gives the same block on every compiler (the test pins its
hash). Every single-bit flip of a test block and every truncation is
refused; structural faults behind a valid hash (a level out of range, a
bad name, a bad type, a misaligned offset, too many levels, a non-integer
INTEGER value, a text without its NUL, bad UTF-8 in the pool) are refused
by name.

### Fuzzing

`tests/fuzz/table_fuzz.c` is a libFuzzer target. Its first byte picks
CSV (with a delimiter, `allow_empty` and one declared type from the next
byte) or a raw block for `ytb_open_view()`. Every table it gets is read back
through every lookup, and a parsed block is viewed again from a copy. The
target writes its own seeds (`-DYSP_FUZZ_SEEDS`: 72 CSV inputs and 8
blocks); no corpus is committed.

| Run | Inputs | Result |
|---|---|---|
| MSVC 19.44, `/fsanitize=fuzzer,address`, 1351 s from the seeds | 5,906,077 | no crash and no timeout; 3208 coverage features, 958 corpus files |
| The 958 corpus files replayed on Linux gcc 11.4, ASan and UBSan, as C11 and as C++17 | 958 each | no report |

### Mutations

`tests/mutate/table.toml` holds 25 mutants of ysp/table.h: 9 in the
number parser (the fast-path bounds, ties, the sticky bits, subnormal
rounding, the 19-digit limit, the sign of -0, the quotient width, the
middle-path bound), 12 in the CSV parser (quote undoubling, CRLF, a lone
CR, the BOM, quotes in unquoted fields, short rows, empty rows, UTF-8,
the field bound, `allow_empty`, type inference, NaN levels) and 4 in the
view checks (a level equal to the level count, the hash, a text's NUL,
pool UTF-8). Run with `YTB_TEST_NUMBERS=100000`, the test kills 24.
The survivor is equivalent and marked so: n-08 moves |E| = 27 from the
128-bit path to the big-integer path, and both are exact.

Two mutants survived the first full run and showed gaps in the test,
since closed: n-04 (sticky bits dropped) needs an exact quotient with a
half bit and set bits below it, which the random corpus missed, so the
fixed strings gained `9007199254740993.5` and three like it; c-02 (CRLF
consumed as CR only) gave the same rows, so the test now checks the line
number of an error after CRLF records.

## Cost

All on the Iris Xe laptop, AC, the measurement lock held (`guard.sh time
trials`), CPU load 16 % (gcc) and 0 % (MSVC) when the lock was taken,
2026-10-07. `examples/trials/bench.c`, 21 rounds, MinGW gcc 16.1 -O2 and
MSVC 19.44 /O2. The inputs are 10,000 rows x 8 columns made from a fixed
seed: words of 3 to 12 letters (strings), `%.17g` doubles as pandas
writes them (numbers), and 30 % quoted fields with commas and line breaks
(quotes).

| Input | Bytes | gcc median ms (min to max) | gcc MB/s | MSVC median ms (min to max) | MSVC MB/s |
|---|---|---|---|---|---|
| strings | 680,867 | 3.77 (3.59 to 4.39) | 181 | 3.61 (3.41 to 4.16) | 188 |
| numbers | 1,511,152 | 8.89 (8.55 to 9.52) | 170 | 15.57 (15.34 to 17.39) | 97 |
| quotes | 1,097,264 | 5.49 (5.36 to 5.86) | 200 | 5.03 (4.78 to 5.65) | 218 |

| Block | Block bytes | Arena the parse needs | gcc view median ms (max) | MSVC view median ms (max) |
|---|---|---|---|---|
| strings | 2,116,104 | 3,577,304 | 0.261 (0.345) | 0.210 (0.291) |
| numbers | 2,951,456 | 5,237,864 | 0.379 (0.537) | 0.305 (0.492) |
| quotes | 2,439,328 | 4,219,496 | 0.293 (0.356) | 0.280 (0.454) |

The number column is where the compilers differ: 17 significant digits
with exponents up to 27 take the 128-bit path, which gcc does with
`__int128` and MSVC with 32-bit pieces. A conditions file of a few
hundred rows parses in well under a millisecond on either. The view is
the cost a pack pays at load: the hash, the UTF-8 check of the pool and
the structural checks, about 0.3 ms for 80,000 cells.

Earlier versions of the code were slower in indicative runs, not under
the lock, and the changes were kept because of that: the number parser converted twice (17 MB/s);
the view hashed with FNV-1a and checked every text with `memchr` (2.5 ms
for the strings block).

## Open, close and negative reasons (v0.2.0)

v0.2.0 applies the repository conventions (README.md, Conventions):

- **The parse and the view are opens.** They fill a handle and check what
  they read, so they take the open verb: `ytb_open_csv()`,
  `ytb_open_csv_file()` and `ytb_open_view()`. `ytb_close()` zeroes the
  handle. It releases nothing, because the block is in the caller's arena
  or in a pack, but every open has a close.
- **The number reasons are negative.** `YTB_NUM_SYNTAX`, `YTB_NUM_DIGITS`
  and `YTB_NUM_RANGE` are -1, -2 and -3, so `rc < 0` is a failure here as
  in every other header. Every caller compared with the names, so no caller
  changed.

The parser, the block format and the hash are v0.1.0's: the pinned block
hash in `tests/adapt/table_test.c` did not change. Checked on 2026-10-10
(Windows 11, MSVC 19.44 /W4 /WX as C and C++17, MinGW-w64 gcc -Werror),
with new checks for `ytb_close()` on a zeroed handle, after a failed open
and twice. The fuzz target builds against the new names.

## 256 columns (v0.3.0)

- Problem: a trial row of the experiment definition
  ([experiment_format.md](experiment_format.md) 2.10, gap G10) has seven
  columns per recorded stimulus beside the session's, the response's and
  the template's, so a template with many recorded stimuli passes 64
  columns. ysp/trials.h's data writer keeps to this header's bound, so
  its files read back.
- Options: remove the bound (the parse's column state and the declared
  types into the arena, `types[]` a pointer); raise it.
- Decision: raise `YTB_MAX_COLUMNS` to 256. The block format already
  stores the column count in 32 bits, so no byte of a block changes and
  a block of up to 64 columns hashes as before. What grows with the
  bound: `ytb_csv_desc.types[]` (1072 to 4144 bytes), the parse's stack
  (16 bytes a column, 1 to 4 KB) and ysp/trials.h's level pointers (8
  bytes a column). Removing the bound would change the desc's API for
  every caller, and nothing yet asks for more than 256.
- Checked on 2026-10-21: 256 columns parse, view, and read back by name;
  257 are refused with the bound in the message (tests/adapt/
  table_test.c). Mutants b-01 (one column past the bound parses) and b-02
  (the view refuses the widest block) are caught. The pinned block hash
  did not move.

## Not done

- No big-endian host: parse and view refuse with a message. No supported
  platform is big-endian.
- No run on macOS.
- The hash finds corruption, not forgery; a pack from an untrusted source
  is checked for memory safety, not authenticity.
- No `n/a` or other missing-value tokens (BIDS writes `n/a`): the pack
  tool converts them.
