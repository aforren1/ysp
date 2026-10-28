# Mutation tests

A mutant is one deliberate fault in a header: a literal edit of the code.
A test kills a mutant when the test fails on the mutated header. A mutant
that survives shows a fault that the test cannot see.

`mutate.py` runs the mutants. Each `*.toml` file in this directory holds
the mutants of one header.

The runner does not change the headers in the repository. It copies the
headers and `tests/adapt` into a work directory, edits the copy, builds
the test against the copy and runs it. Other people can build against the
repository while a run continues.

The runner is not part of CI or ctest, because a full run takes too long
(see [Run time](#run-time)).

## Run the mutants

Use [uv](https://docs.astral.sh/uv/). The runner needs Python 3.11 or
later and the standard library only.

From the repository root:

```sh
uv run tests/mutate/mutate.py                       # every list
uv run tests/mutate/mutate.py color.toml            # one list
uv run tests/mutate/mutate.py gfx.toml --only v04-06 v04-07
uv run tests/mutate/mutate.py --check               # anchors only, no builds
uv run tests/mutate/mutate.py timeline.toml -j 4    # 4 mutants at a time
```

Options:

| Option | Effect |
|---|---|
| `--check` | Checks that each anchor occurs in the current headers the expected number of times. Builds nothing. Use it after you change a header. |
| `--only ID ...` | Runs only these mutants. |
| `-j N` | Builds and runs N mutants at the same time. Default 1. Do not use it with `gfx.toml`: the GPU checks share one GPU. |
| `--work DIR` | Puts the copies and the builds in DIR. Default: a new temporary directory, removed at the end. |
| `--keep` | Keeps the temporary directory. |
| `--guard CMD` | Runs each test run (baseline and mutant) through CMD, one at a time. See [Window mutants](#window-mutants). |

The runner prints one line for each mutant and a summary for each list.
The exit code is 0 when each result agrees with the mutant's `expect`
value.

### Results

| Result | Meaning |
|---|---|
| `killed` | The test found the fault: a check failed that passed on the baseline, or the exit code changed (for example, a crash). |
| `survived` | The test passed, or failed only the checks that also failed on the baseline. |
| `timeout` | The test did not end in the test's `timeout`. A timeout agrees with `expect = "killed"`. |
| `build-failed` | The mutated copy did not compile. Change the replacement so that it compiles. |
| `no-match` | The anchor did not occur the expected number of times. The header changed: update the anchor (see [Add a mutant](#add-a-mutant)). |
| `skipped` | The unmutated baseline did not compile. |

The baseline is the unmutated test, built and run once for each test and
environment. If the baseline fails some checks (for example, on a GPU
with a known fault), the runner ignores those checks. It compares failed
checks by file line, not by the values in the message, because measured
values change from run to run.

### Builds

| List | Compiler | Command |
|---|---|---|
| `gfx.toml` | MSVC (`cl`), found with `vswhere` | `cl /nologo /O1 /W3 /D_CRT_SECURE_NO_WARNINGS /I<copy> tests/adapt/gfx_test.c` |
| `color.toml` | gcc | `gcc -std=c99 -O2 -w -I<copy> tests/adapt/color_test.c -lm` |
| `screen.toml` | gcc | `gcc -std=c11 -O1 -w -I<copy> tests/adapt/screen_test.c -lm` |
| `timeline.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/timeline_test.c -lm` |
| `rt.toml` | gcc | `gcc -std=c11 -O2 -w -DYRT_TEST_FIT_ONLY -I<copy> tests/adapt/rt_test.c -lm` (the device clock fit only) |
| `box.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/box_test.c -lm` |
| `device.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/device_test.c -lsetupapi -lm` (Windows) |
| `video.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/video_test.c -lm` |
| `json.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/json_test.c -lm` (`YJS_TEST_NUMBERS=50000`) |
| `rigfile.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/rigfile_test.c -lsetupapi -lm` (Windows) |
| `parallel.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/parallel_test.c -lm` (Windows: the pulses run on stand-in `Out32` and `Inp32`) |
| `net.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/net_test.c -lsetupapi -lm` (Windows; `YNET_TEST_SECONDS=1`; a fake liblsl, none needed) |
| `stair.toml` | gcc | `gcc -std=c99 -O2 -w -DYST_MAX_TRIALS=128 -DYST_MAX_REVERSALS=32 -I<copy> tests/adapt/stair_test.c -lm` |
| `quest.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/quest_test.c -lm` |
| `aep.toml` | gcc | `gcc -std=c11 -O2 -w -I<copy> tests/adapt/aep_test.c -lm` (`YAEP_TEST_ONLY=params`: the parameter table and snapshot outcome checks only) |

These are the compilers of the original runs. The runner finds gcc in
this order: the `YSP_MUT_GCC` environment variable,
`C:\tmp\winlibs\mingw64\bin\gcc.exe`, then `gcc` on the `PATH`. To use a
different `vcvars64.bat`, set `YSP_MUT_VCVARS`.

### Environment

| List | Needs |
|---|---|
| `gfx.toml` | Windows, a D3D11 GPU, and ANGLE (`libEGL.dll`, `libGLESv2.dll`) in `YSCR_ANGLE_DIR`. Default: `C:/Program Files/Docker/Docker/frontend`. |
| `video.toml` | For the `mf-` mutants: Windows (Media Foundation) and the clips of `tests/media/make_video_clips.sh` in `build-vid/media`, or `YVID_TEST_MEDIA` set to their directory. |

### Shared machine

Before each build and each test run, the runner looks for the quiet flag
`C:\tmp\psy-quiet`. While the flag exists, the runner does no work and
looks again every 60 s. Before each mutant, the runner also looks for the
measurement lock `C:\tmp\psy-measure.lock` and waits while it exists. The
runner never makes or removes these files. To use other paths, set
`YSP_QUIET_FLAG` and `YSP_MEASURE_LOCK`.

### Window mutants

The tests that open real windows (`screen.toml`'s `window` test) must
run one at a time on the machine, under the timing guard
`psy-guard.sh time`. Do not run the runner itself under
`psy-guard.sh time`: the guard holds the measurement lock, and the
runner waits for that lock to go before each mutant, so the run waits
forever.

Give the guard command to `--guard` instead. The runner then builds
outside the lock and runs each test through the guard, which takes the
lock for that run only:

```sh
uv run tests/mutate/mutate.py screen.toml -j 1 \
    --guard "bash C:/tmp/psy-guard.sh time mutate-window" \
    --only wt-20 wt-21 fg-08 bf-01
```

- The runner appends its own command to CMD: the test with its
  `timeout`. The timeout starts when the guard starts the test, not
  while the guard waits for a quiet machine.
- The runner finds the first word of CMD on the `PATH`. On Windows,
  Git's `bash.exe` must come before `C:\Windows\System32\bash.exe`
  (WSL), as it does in a Git Bash shell.
- Use forward slashes in CMD. The runner splits CMD as a POSIX shell
  does, so a backslash is an escape.
- Use `-j 1`: two windows at a time take the foreground from each other.
- Some cases need the foreground (keys, the next screen's raise). A run
  started from a program in the background can lose it: those checks
  then skip, and the mutant survives. Start the run from a terminal in
  front, and do not type in other programs while it runs.

## Add a mutant

1. Find the code that you want to break. Copy the exact text, with its
   indentation, as the anchor. Use enough text to make the anchor occur
   one time only.
2. Add a `[[mutant]]` table to the list of the header:

   ```toml
   [[mutant]]
   id = "07"                     # unique in the file; never reuse an id
   name = "cbrt as pow"          # short name for the report
   test = "color"                # a [test.NAME] table in the same file
   why = "Oklab needs a real cube root; pow() returns NaN for a negative cone response."
   find = 'l[k] = cbrt(l[k] + cx->ok_black[k]);'
   replace = 'l[k] = pow(l[k] + cx->ok_black[k], 1.0 / 3.0);'
   ```

3. Run `uv run tests/mutate/mutate.py color.toml --only 07`.
4. If the mutant survives, add a check to the test that kills it, or
   record why no check can (see `expect`).

Use TOML literal strings (`'...'`, or `'''...'''` for more than one line)
for code: they keep backslashes as they are. In a `'''` string, the
newline directly after the opening quotes is not part of the text.

### Mutant fields

| Field | Required | Meaning |
|---|---|---|
| `id` | yes | Unique in the file. The ids follow the tables in the manuals. |
| `name` | yes | A short name. |
| `test` | yes | The `[test.NAME]` table that must kill the mutant. |
| `why` | yes | Why the fault matters: what a user would get wrong. |
| `find` | yes | The exact original text. Line ends are `\n`. |
| `replace` | yes | The replacement text. |
| `count` | no | The number of times `find` must occur. All of them are replaced. Default 1. |
| `file` | no | The header to edit, when it is not the file's `header`. |
| `env` | no | Environment variables for this mutant's test run (for example, `YGFX_TEST_PARTS`). |
| `expect` | no | `killed` (default) or `survived`. Use `survived` for a control or for an equivalent mutant, and give the reason in `note`. |
| `note` | no | Why the mutant is expected to survive. |
| `[[mutant.also]]` | no | More edits for the same mutant: `find`, `replace`, and optionally `count` and `file`. |

### Test fields

| Field | Meaning |
|---|---|
| `source` | The test source, relative to the repository root. |
| `compiler` | `gcc` or `msvc`. |
| `flags`, `libs` | Compiler arguments before the source, and libraries after it. |
| `timeout` | Seconds for one test run. Default 600. |
| `env` | Environment variables for each run. |
| `env_default` | Environment variables that the runner sets only when they are not set already. `{repo}` is the repository root. |
| `unset` | Environment variables that the runner removes. |
| `ignore` | Regular expressions. The runner ignores the failure lines that match. |

## When a header changes

Run `--check` after a change to a header. For each `no-match` result:

- If the code moved or changed but the fault still makes sense, change
  `find` and `replace` to the current code. Keep the `id`.
- If the code is gone, remove the mutant and record the removal in the
  header's manual.

## Run time

A full run of the 144 mutants took about 61 minutes on a laptop with an
Intel Core i7-1360P (2026-10-06). That time includes pauses for the
measurement lock.

| List | Mutants | `-j` | Time |
|---|---|---|---|
| `color.toml` | 33 | 4 | 4 min |
| `screen.toml` | 12 | 4 | 4 min |
| `timeline.toml` | 43 | 4 | 9 min |
| `video.toml` | 21 | 3 | 19 min (one test run with the clips takes about 70 s) |
| `gfx.toml` | 35 | 1 | 25 min (about 42 s for each mutant) |
| `rt.toml` | 27 | 4 | 27 s (2026-10-08) |
| `box.toml` | 13 | 4 | 4 s (2026-10-08) |
| `device.toml` | 12 | 4 | 17 s (2026-10-08) |
| `stair.toml` | 17 | 4 | 15 s (2026-10-21) |
| `quest.toml` | 8 | 4 | 15 s (2026-10-21) |
| `aep.toml` | 10 | 4 | 88 s (2026-10-21) |
