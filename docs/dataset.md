# ysp/dataset.h

Status: v0.1.0, 2026-10-21. The header's manual is the reference (THE
FOLDER, SESSION.JSON, THE TABLES, THE ZIP). This page records why the
header exists, where it lives, the decisions, and the results.

## Why a header

The player writes a session's data as a folder of CSV files and
`session.json` (experiment_format.md, D9 and 2.10). Gap G13 was that
nothing wrote that folder. The player does not exist yet, so the writer
is library code with tests now, and the player's later work is to call
it.

### Where it lives (the user confirmed the name and the prefix, 2026-10-21)

No document placed the writer. rig_spec 6 and experiment_format.md 2.10
say only "the player" writes each file. trials.md (v0.4) gave the trial
table to `ysp/trials.h` and left "the data file" to the player.

| Option | Verdict |
|---|---|
| A new header, `ysp/dataset.h`, prefix `yds_` | Chosen. The native player, the WebAssembly player and C from the designer's eject path all need the same writer. A header gets the repository's compile checks, tests, mutants and CI on every platform. `yds_` is free in the README's Prefix column |
| In `ysp/trials.h` | Refused. That header is pure computation and stdio. The data set needs `ysp/json.h`, folders and an atomic rename, and has no part in trial sequencing |
| Player code | Refused for now. No player exists, so the code would have no build and no test, and the eject path could not use it |
| Other names | `ysp/session.h` (`yses_`): "session" is already the trial handler's word for one run of `ytr_trials`. `ysp/data.h`: too wide. `ysp/record.h`: "record" is a flip record. "Data set" is D9's own term |

The header sits in the `ysp::methods` CMake group beside `ysp/trials.h`,
whose writer it uses.

## How-to: write a session's data set

1. Build the player's keys of `session.json` in a `ysp/json.h` arena
   (the experiment ID, the pack ID, the rig profile's hash, the session
   fields, the seed, the versions). The writer adds `format`, `version`,
   `closed`, `start_utc`, `rows` and `write_errors`, and refuses those
   keys from you (except `start_utc`).
2. Declare the trial row's columns as for `ytr_data` (`YTR_COL`,
   `YTR_COL_SET`, `YTR_COL_TEXT`), then open:

   ```c
   static yds_dataset ds;                 /* about 770 KB: static storage */
   yds_desc d = { 0 };
   d.dir = "data/P07_20261021T140312Z";   /* must not exist; its parent must */
   d.session = session;                   /* copied */
   d.trial_cols = cols;
   d.n_trial_cols = n_cols;
   if (!yds_open(&ds, &d)) { fputs(yds_error(&ds), stderr); return 1; }
   ```

3. Write rows as they happen. Include `ysp/screen.h`, `ysp/input.h`,
   `ysp/timeline.h` or `ysp/rt.h` before `ysp/dataset.h` to get the
   adapters:

   ```c
   for (i = 0; i < f.n_done; i++) {
       yds_frame fr;
       yds_frame_from_screen(&fr, &f.done[i], trial_index);
       yds_write_frame(&ds, &fr);
   }
   yds_write_trial(&ds, &row);            /* after ytr_update() */
   ```

   Use `yds_trials_writer(&ds)` for `ytr_data_set_*()` and
   `yrsp_data_set()`, and `yds_stream()` for one more table
   (`streams/<name>.csv`, an RDK's steps or a device stream).
4. Log a live change with `yds_session_push(&ds, "changes", change)`.
   Each call rewrites `session.json` atomically.
5. End the session:

   ```c
   if (yds_finish(&ds) < 0) fputs(yds_error(&ds), stderr);
   yds_close(&ds);
   ```

   `yds_close()` alone also finishes, so a cleanup path that only closes
   still marks the data set closed.

`examples/dataset/session.c` is a complete program that does this for a
simulated session.

## How-to: make the web form

On the web the folder is in Emscripten's in-memory file system. After
`yds_finish()`, call `yds_zip(&ds, NULL, write, ctx)` and post or
download the bytes that `write` gets, or give a path. The zip is stored
(no compression), with the folder's name as its top folder.

## How-to: read a data set

- pandas: `pd.read_csv("trials.csv")`; R: `read.csv("trials.csv")`.
  Both read every file with their defaults, line breaks in quoted text
  included.
- `ysp/table.h`: `ytb_open_csv()` with `allow_empty`, up to its 32767
  rows. `frames.csv` of a long session has more rows than that.
- Check `session.json` first. `"closed": false` means the session did
  not end normally; the rows in the files are the rows written before
  the end. `rows` gives the count of each file at the finish, and
  `write_errors` names a file that lost rows.

## Decisions

| Question | Decision | Why |
|---|---|---|
| CSV text | Each table is a `ytr_data` with no session (`desc.trials` NULL) | D9 names the trials writer; one writer means one text rule for numbers, missing values, quoting and `_s` names, and its tests and mutants cover every file. No fork |
| A row reaches the OS | When the write call returns (`YTR_FLUSH_ROW`); `YTR_FLUSH_SYNC` on request | The trials writer's measured rule: a crash keeps every row. The cost is below |
| An existing folder | Refused (`YDS_ERR_EXISTS`); its parent must exist | A data set never writes into another one. Making parents would hide a mistyped path |
| A failed open | Checks the trial columns before the folder exists, and removes what it made | A refused column leaves nothing to clean up |
| `session.json` | `ysp/json.h`'s canonical form and one LF; `"closed": false` from the open, `true` at the finish | D9. The canonical form gives one byte string per content |
| Atomic rewrite | Write `session.json.tmp`, sync it to the disk, rename it over | A reader or a crash sees the old file or the new one. The sync is before the rename, so a power cut cannot leave a renamed empty file |
| Rename over a file on Windows | `MoveFileExW(MOVEFILE_REPLACE_EXISTING \| MOVEFILE_WRITE_THROUGH)`, 5 tries 10 ms apart | The C library's `rename()` refuses an existing target on Windows. `MoveFileExW` with REPLACE_EXISTING is one rename on the volume (the method `ysp/rigfile.h` uses for the rig profile). It fails while another program holds the file without FILE_SHARE_DELETE, which virus scanners do for milliseconds; the retries cover that. `ReplaceFileW` was not used: it fails in more cases (another volume, attributes) and gains nothing here. POSIX `rename()` replaces atomically; the folder is synced after it |
| Writer's keys | `format`, `version`, `closed`, `start_utc` (when absent), `rows` and `write_errors` | `rows` is the check that a file is whole, as the trials sidecar's `rows=N`; `write_errors` says which file has a hole |
| frames.csv's first column | `frame`, not 2.10's `index` | events.csv has a `frame` column; one name joins the two tables. experiment_format.md 2.10 now says `frame` |
| events.csv | One table, a `kind` column, a union of columns | 2.10 lists landings, responses, device events, outputs and marks in one file. A `kind` value per source keeps one file and one reader; an empty cell is a field that the kind does not have |
| `trial_index` in frames.csv and events.csv | Yes | It joins a frame or an event to its trial row without a time window |
| Missing values in frames, events, clock | `_init()` sets each field to the trials writer's missing value; the caller sets what the row has | A zeroed struct would write 0, a value, in every field the caller did not set |
| Stream names | The column rule and unique in more than case | Each name is a file name; on Windows `dots.csv` and `DOTS.csv` are one file |
| The zip | Stored, CRC-32, UTF-8 names (bit 11), every date 1980-01-01, the folder's name as the top folder; no zip64 | The pack tool's zip knowledge (`pack/pack_tool.c`), without its alignment and private records. Fixed dates make the bytes depend on the files only (the test pins a digest). A data set of 4 GiB or more does not fit wasm32's memory; on a rig the folder is the data set |
| Memory | A `ytr_data` per table in the handle (16 streams: about 770 KB with ysp/trials.h v0.6, 380 KB with v0.5); `session.json`'s values in a heap arena | No allocation per row. The arena grows by the values set, which happens at the open, per live change and at the finish |
| The handle's size on trials v0.6 | Accepted (option c), with the open zeroing a stream's writer only when `yds_stream()` makes it | trials v0.6 doubled `ytr_data` (256 columns: 18,760 to 38,224 bytes) and the handle (377,496 to 766,776). Measured below: no row, open or close got slower; the cost was the working set, because the open zeroed the whole handle. Zeroing the used parts only brings the open's working set from 936 to 948 KB down to 340 to 352 KB, below v0.5's 564 KB, with no API change. A lower YDS_MAX_STREAMS default would cut capacity for nothing the lazy zeroing does not already save. Caller storage for `ytr_data`'s columns (a trials API change) saves address space only, which a static handle does not spend |

## Results

On 2026-10-21, Windows 11 (i7-1360P), MinGW-w64 gcc 16.1, MSVC 19.44,
WSL2 Ubuntu 22.04 gcc 11.4, emcc under node 24.19.

| Check | Result |
|---|---|
| `tests/adapt/dataset_test.c` | On ysp/trials.h v0.6.0: 561 checks pass on MSVC (Release, C) and MinGW (C11), in ctest with every other test (76 of 76 and 81 of 81); 555 on WSL2 gcc at -O2 and at -O1 with ASan and UBSan (no Windows lock checks); 536 under emcc and node (no child process). The ysp_trials Python binding's 163 tests pass on the merged tree. The compile checks build as C99, C11, C11 -O3 and C++17 with warnings as errors on gcc, and with a system header before the header under -std=c11 |
| The golden session | `session.json` at the open and at the finish, the four tables and a stream, byte for byte (`tests/adapt/dataset_golden.h`); every CSV read back by `ysp/table.h`; `session.json` parses and is its own canonical form |
| Crash | A child process writes rows and a live change, then leaves with `_Exit()` (no finish, no stdio flush): `session.json` parses with `"closed": false` and no `rows`, no temporary file, every row of every file reads back. The same in-process while the data set is open |
| Atomic rewrite | A folder in the place of `session.json.tmp`: the rewrite fails, the old file stays byte for byte. On Windows, `session.json` held without FILE_SHARE_DELETE: `YDS_ERR_IO` after the 5 tries, the old file whole, no temporary file; the value is written by the next rewrite |
| The zip | Parsed entry by entry in the test (names, sizes, CRC-32, offsets, modes), the same bytes to a file and to memory, the golden digest; Python's `zipfile` (`testzip()`), Info-ZIP `unzip` and PowerShell's `Expand-Archive` extract the golden zip to the same files |
| pandas | pandas 2 reads every golden file with its defaults, the quoted line break included |
| trials v0.6's data-line columns | A session with a variable and a seed: each row's variable text and `trial_seed` (`yds_seed_text()`) equal the trials writer's own data line (`ytr_format_row()`), and read back from trials.csv as text |
| Mutants (`tests/mutate/dataset.toml`) | 33 in all, MinGW gcc 16.1: 32 killed; ds-09 (no sync before the rename) survives as expected, since only a power cut shows it |

The handle's size on ysp/trials.h v0.6 (2026-10-21): the same laptop,
MinGW-w64 gcc 16.1 -O2, under the measurement lock (load 1 %), A and B
alternating, three rounds each; A on trials v0.5.1 (handle 377,496
bytes), B on v0.6.0 (766,776 bytes); medians of each round:

| Call | A | B |
|---|---|---|
| Frame row | 11.9, 11.7, 11.6 us | 11.7, 9.0, 9.5 us |
| Event row | 12.9, 11.7, 11.9 us | 11.7, 11.9, 11.7 us |
| Trial row | 9.7, 8.4, 9.2 us | 8.9, 8.9, 10.4 us |
| Open, empty data set (n = 20) | 12.9, 11.9, 12.2 ms | 13.3, 15.4, 13.6 ms |
| Close, empty data set (n = 20) | 10.8, 9.4, 9.7 ms | 11.0, 11.8, 10.2 ms |
| session.json rewrite (n = 200) | 9.5, 8.1, 8.5 ms | 8.4, 8.5, 8.2 ms |

The open and the close are the syncs of `session.json` to the disk;
their spread between rounds (1 to 3 ms) is larger than any difference
between A and B. The working set that the open adds (a static handle,
`GetProcessMemoryInfo()`, three runs): 936 to 948 KB with B zeroing the
whole handle, 340 to 352 KB with B zeroing the used parts (the fix), 564
KB with A.

Costs (`examples/dataset/bench.c`, 2026-10-21): the Iris Xe laptop
(i7-1360P) on AC, Windows 11, its internal SSD (NTFS), MSVC 19.44 /O2,
under the measurement lock (load 1 % at its start). Per call, median
(p99, max):

| Call | n | Cost |
|---|---|---|
| `yds_write_frame()`, `YTR_FLUSH_ROW` | 10000 | 12.0 us (25.3, 193) |
| `yds_write_event()`, `YTR_FLUSH_ROW` | 10000 | 12.3 us (21.1, 410) |
| `yds_write_trial()`, 6 columns | 1000 | 9.2 us (47.1, 225) |
| `yds_write_frame()`, `YTR_FLUSH_SYNC` | 500 | 2.9 ms (7.8, 12.5) |
| `yds_session_set()`: the atomic rewrite | 200 | 8.2 ms (13.1, 14.8) |
| `yds_open()` | 1 | 15.7 ms |
| `yds_finish()` | 1 | 12.1 ms |
| `yds_zip()` of the 1.6 MB folder to a file | 1 | 14.4 ms (114 MB/s) |

A frame row costs about 12 us, under 0.1 % of a 16.7 ms frame; its
worst case (0.2 to 0.4 ms) is a write call that the OS held. Write frame
rows from the frame loop only with that worst case in the budget. A
player that writes them between trials instead must copy them out of
`yscr_frame.done`, which holds them until the next `begin()` only. The
rewrite of `session.json` costs 8 ms (the sync to the disk and the
rename), so it belongs between trials, never in a trial's frames; a
session has a few (the open, the live changes, the finish).

## Not done

- No fuzz target: the header parses no input. `session.json`'s values
  come in as a built `ysp/json.h` tree, the rows as structs, and the zip
  reads back the files that the writer wrote.
- No zip64: a file or a zip of 4 GiB or more is refused.
- A write failure on a real disk is simulated in the test (the trials
  writer's failure flag), not made; the trials writer's own tests fail a
  write function.
- A failed open after the folder exists (a table that cannot be
  created) is not reached by a test; its clean-up has no mutant.
- No adapter for `ysp/device.h`'s output records (`YDEV_REC_OUT`) or an
  LSL inlet's samples: the player fills `yds_event` and stream rows from
  them.
- No run on macOS or on a Linux disk under power loss.
