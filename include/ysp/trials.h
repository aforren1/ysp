/* ysp/trials.h - v0.3.0 - public domain single-header trial sequencing library
 *
 *   The layer above the adaptive methods: which trial comes next, and what
 *   happened on it. Conditions and repetitions (the method of constant
 *   stimuli), sequential, random and constrained-random orders ("no more
 *   than three of the same orientation in a row", "a catch trial at most
 *   once in any six trials"), interleaved adaptive tracks (staircases,
 *   QUEST+, GP handles owned by the caller), blocks, practice trials, catch
 *   trials, re-queued trials, per-condition tallies, and a history that
 *   replays. PsychoPy's TrialHandler and MultiStairHandler, as one C
 *   header with no heap. From v0.2: conditions files and trial lists as
 *   tables (CSV or a pack), order lists, draws with replacement, subsets,
 *   blocked and alternating groups in Latin-square order, units that run
 *   back to back ("b always follows a"), first-order transition balance,
 *   and a rules text that sets all of it by column and level names.
 *   From v0.3: a data writer, one CSV row per trial with the session's
 *   columns and the caller's (PsychoPy's addData without a heap).
 *
 *   Written in the single-header style of the stb / sokol libraries. Pure
 *   computation: no threads, no heap, and no I/O but the data writer's
 *   (DATA FILE), which opens, writes and closes its file with the C
 *   library, or hands its bytes to a function of the caller's. REQUIRES
 *   ysp/table.h beside it (v0.2; its implementation is compiled with this
 *   one's, as ysp/gfx.h does ysp/color.h's), and otherwise only the C
 *   standard library (snprintf for the format functions; stdio, and
 *   _commit() or fsync() for YTR_FLUSH_SYNC, for the writer). It does not
 *   include the method headers: a track is an opaque pointer and an
 *   is-done callback, so this header interleaves anything and knows nothing
 *   about stimuli or levels.
 *
 *   Targets every platform the compiler does. C99 is the floor: it builds as
 *   C99, C11 and C++17, and in the C dialect MSVC compiles by default.
 *
 *   ---------------------------------------------------------------------
 *   CHANGELOG
 *   ---------------------------------------------------------------------
 *   v0.3.0 - the data writer (DATA FILE): ytr_data_open() with a column
 *          list declared once (int, int64, double, bool, string, char
 *          array, a time in int64 nanoseconds written as seconds, and
 *          constant text such as the participant), each bound to a field
 *          of the caller's row struct (YTR_COL) or set per row by index
 *          (YTR_COL_SET, ytr_data_set_*()); ytr_data_write() writes the
 *          session's columns (ytr_format_row()'s) and then the caller's,
 *          one CSV row per trial; ytr_data_close(). RFC 4180 quoting, LF
 *          line endings, an empty field for a missing value, bad UTF-8
 *          replaced by U+FFFD, doubles in the fewest digits that read back
 *          exactly, '.' whatever the locale: a file that ysp/table.h reads
 *          back as written. Each row reaches the OS before write()
 *          returns (YTR_FLUSH_ROW), or the disk (YTR_FLUSH_SYNC). An
 *          optional sidecar of key=value lines (meta_path) with a closing
 *          line. Works without a session. No allocation: the column
 *          list and an 8 KB row buffer live in the handle.
 *          - YTR_ERR_IO; ytr_strerror() names it.
 *          - The session's schedule, draws, history, records, tallies,
 *            format lines (but the version token) and snapshot bytes are
 *            v0.2.1's: the 600 v0.1.1 and 320 v0.2.0 pins still match.
 *   v0.2.1 - jitter: a random duration per trial (a foreperiod, an ITI,
 *          an SOA), drawn from the session's generator in a fixed order, so
 *          restore() and load() reproduce it, and logged in the data line
 *          (JITTER). Uniform on an interval, uniform over a list
 *          (ytr_choice()), and an exponential truncated to an interval
 *          (a non-aging foreperiod); any of them snapped to whole frames of
 *          a rate num/den. In seconds, also as int64 nanoseconds and frame
 *          counts. Every draw is integer arithmetic on the variate's 53
 *          bits (a fixed-point logarithm for the exponential), so it is the
 *          same nanosecond on every C library, compiler and flag set.
 *          desc.jitters[] with per-condition intervals from table
 *          columns, ytr_jitter() and ytr_jitter_index(); call-level
 *          ytr_jitter_draw(), ytr_jitter_map(), ytr_jitter_check();
 *          the rules statement `jitter`.
 *          - Every v0.2.0 desc gives the same schedule, draws, history,
 *            records, tallies, format lines (but the version token) and
 *            snapshot bytes as v0.2.0: tests/adapt/trials_pins_v02.h
 *            holds the hashes of 320 sessions (16 v0.2 designs x 20 seeds)
 *            made by v0.2.0, beside the 600 of v0.1.1.
 *          - Snapshot format 3 for a desc with jitters (format 2 plus the
 *            jitters and their draws); formats 1 and 2 as before.
 *          - The handle grew by 8 bytes per trial per YTR_MAX_JITTERS (4):
 *            243344 bytes at the defaults on a 64-bit ABI. Define
 *            YTR_MAX_JITTERS lower to save it.
 *   v0.2.0 - tables, sampling, groups, units, balance, Latin squares, rules
 *          text. Every v0.1 desc gives the same schedule, draws, history,
 *          records, tallies, format lines (but the version token) and
 *          snapshot bytes as v0.1.1: tests/adapt/trials_pins.h holds
 *          the hashes of 600 sessions (30 designs x 20 seeds) made by
 *          v0.1.1, and the test checks them.
 *          - REQUIRES ysp/table.h. desc.table: a table's rows are the
 *            conditions and its columns the factors (TABLES).
 *          - YTR_ORDER_LIST and YTR_ORDER_WITH_REPLACEMENT, desc.subset
 *            (ORDER LISTS, DRAWS, SUBSETS); desc.groups (GROUPS).
 *          - Structural rules ytr_followed_by(), ytr_preceded_by(),
 *            ytr_chunk() (STRUCTURE: UNITS); ytr_balance(),
 *            ytr_balance_no_repeat(), ytr_balance_flags() (BALANCE);
 *            ytr_latin() (LATIN SQUARES); ytr_rules() and
 *            ytr_format_rules() (GRAMMAR).
 *          - YTR_FLAG_LEADIN on the lead-in of a balanced order. No
 *            field was added to ytr_trial_info, so a positional
 *            initializer of it stays warning-free (the history flag and
 *            rep -1 tell the lead-in).
 *          - Snapshot format 2 for a desc with any v0.2 field; format 1,
 *            the v0.1.1 bytes, for any other. load() takes the format the
 *            desc implies.
 *          - format_meta() adds the v0.2 keys (the table's hash among them)
 *            after the v0.1 ones, and only for a desc with a v0.2 field;
 *            format_header() and format_row() add a `leadin` column only for
 *            a balanced order with a lead-in.
 *          - The handle grew by 19 KB (scratch for open() and the units'
 *            run-time moves, the table's level pointers): 110528 bytes at the
 *            defaults on a 64-bit ABI.
 *          - A desc that puts an out-of-range value in an enum field is
 *            still refused; in C++ such a value must lie in the enum's bit
 *            range or reading it is undefined (the test now respects that).
 *   v0.1.1 - hardening for newer compilers (gcc 13 and 16, clang 18 at -O3
 *          with warnings as errors), no change to any order, draw or
 *          snapshot byte. Every loop over a fixed-size array is bounded by
 *          the array's size as well as its count, every memmove size is
 *          visibly nonnegative and inside the arrays, and open() copies
 *          desc.cond_reps and desc.warmup_conditions into the handle,
 *          reading each entry once, so neither needs to outlive open().
 *          open() now rejects n_warmup_conditions above
 *          YTR_MAX_CONDITIONS. The handle grew by 4 KB.
 *   v0.1.0 - first implementation. Defining YSP_TRIALS_IMPLEMENTATION now
 *          compiles the library instead of failing. The manual gained what
 *          a specification could leave open and an implementation cannot:
 *          the exact generator draw order (RANDOMNESS), the snapshot byte
 *          layout, how blocks are counted and where practice sits, what a
 *          constraint window does at a block boundary (it is clipped),
 *          what ytr_requeue() does to the schedule and the block count,
 *          and the rest of the descs ytr_open() rejects. Behavior that
 *          differs from the v0.0 text:
 *          - Constraints hold on the MAIN sequence (every trial but
 *            practice and warmup, track trials included as trials with no
 *            level), and next() enforces them at run time as well as at
 *            open: a scheduled trial that would break one gives way to a
 *            track trial, or to a later scheduled trial that fits. Without
 *            that, the second USAGE example (one catch condition and three
 *            tracks) could not be expressed. With tracks, a repair that
 *            runs out of swaps is therefore not an open() failure.
 *          - The repair swaps the offending trial with the first of up to
 *            64 slots, from a random start, that leaves both slots legal,
 *            not with a blind random later slot: the blind swap failed
 *            about 15% of easy designs (3 rows x 10, no immediate repeat)
 *            by getting trapped at the end of the schedule.
 *          - YTR_ANY_LEVEL works for max_in_window and min_gap as well
 *            as max_run. no_transition is clipped at block boundaries like
 *            the others; first_not is about the session's first main
 *            trial only.
 *          - ytr_requeue() records the new outcome YTR_REQUEUE (-2),
 *            and ytr_restore() re-queues on YTR_REQUEUE, not on
 *            YTR_INVALID, so update(YTR_INVALID) and requeue() replay
 *            differently and the history's outcomes feed restore directly.
 *            update() rejects outcomes below YTR_INVALID.
 *          - Practice and warmup trials cannot be re-queued (YTR_ERR_ARG).
 *          - Two history flags: YTR_FLAG_FIRST_IN_BLOCK and
 *            YTR_FLAG_VIOLATION.
 *          - ytr_format_row() and ytr_format_header() add warmup and
 *            after_break columns, and end the line with a newline.
 *          - A record pointer with no records buffer is ignored rather
 *            than an error, and a NULL record zero-fills the trial's slot.
 *          - ytr_condition_at() indexes the practice slots too.
 *   v0.0 - specification. Declarations and the manual, no implementation.
 *
 *   STATUS: v0.3.0, 2026-10-10. The data writer, built and run on Windows
 *   11 with MinGW-w64 gcc 16.1 (C11, C++17, warnings as errors) and MSVC
 *   19.44 (/W4 /WX, C and C++17); not yet run on Linux or macOS (CI's
 *   verdict). The v0.1.1 and v0.2.0 pins still match. tests/adapt/
 *   trials_test_data.h: every type and missing value against fixed text
 *   and read back by ysp/table.h; times at 1 to 9 decimals around the
 *   rounding points; 2000 random doubles read back bit for bit, the
 *   decimal point '.' under a German locale; 400 random strings (commas,
 *   quotes, CR, LF, UTF-8, four kinds of invalid UTF-8) read back with
 *   U+FFFD for each bad byte; a session's rows equal to format_row(),
 *   writes refused before update(); 33 refusals; a write function that
 *   fails once; a UTF-8 file name read after each row while open, and its
 *   sidecar. examples/trials/save_rows.c --sim reads its file back.
 *   Mutations: 22 data-writer mutants in tests/mutate/trials.toml, all
 *   killed (69 in the file). Cost per 26-column row (Iris Xe laptop, AC,
 *   measurement lock, examples/trials/bench.c, median, gcc 16.1 -O2 /
 *   MSVC 19.44 /O2): formatting 4.2 / 1.9 us, with the write to the OS
 *   (YTR_FLUSH_ROW) 15.2 / 14.3 us, with the sync to disk
 *   (YTR_FLUSH_SYNC) 1.67 / 1.71 ms. docs/trials.md has the p99 and
 *   maxima.
 *   v0.2.1 STATUS, kept as written: the jitter release, built and run as
 *   v0.2.0 below (gcc 16.1 C11, C99, C++17; MSVC 19.44 C and C++17; WSL2
 *   gcc 11.4 C11 and C++17 under ASan and UBSan; YTR_MAX_TRIALS 256) and
 *   at YTR_MAX_JITTERS 1. The 600 v0.1.1 pins and 320 new v0.2.0 pins
 *   (tests/adapt/trials_pins_v02.h: 16 v0.2 designs x 20 seeds, made
 *   by v0.2.0) match on every build, so no earlier draw moved.
 *   The jitter draws are integer arithmetic: golden digests of 200000
 *   draws of each of six kinds (tests/adapt/trials_jitter_golden.h)
 *   match under MinGW gcc 16.1 -O0 to -Ofast -march=native
 *   -ffp-contract=fast, MSVC 19.44 /Od to /O2 /arch:AVX2 /fp:fast, WSL gcc
 *   11.4 -O2 to -Ofast -march=native and under ASan and UBSan, each with
 *   and without the compilers' 128-bit types, and in the Python binding;
 *   CI checks them on Linux, macOS arm64, MSVC and MinGW and builds
 *   tests/adapt/trials_jitter_repro.c at -O3 native and -Ofast. The
 *   fixed-point -ln(1 - u) is within 0.71 x 2^-58 of mpmath over 1000000
 *   values (tests/compare/trials_jitter_ref.py). tests/adapt/
 *   trials_test_jitter.h also checks every refusal by name; values
 *   at fixed variates and the frame ends; each distribution against its
 *   CDF (200000 draws each, the snapped exponential 1000000; worst
 *   chi-square at 0.80 of its bound) and the truncated mean; 20000 random
 *   snaps inside their intervals; the draw order against a logged
 *   generator; no draw for the pending trial; restore() and save/load
 *   (format 3) reproducing every draw and data line; per-condition
 *   columns; the rules text round trip. Measured per-frame hazard of the
 *   exponential snapped to 60 Hz, 0.5 to 2.0 s, scale 0.4: within 6.2 % of
 *   flat up to 0.8 s (truncation alone accounts for 5.2 %), within 4.4 %
 *   of the truncated geometric's exact hazard; flat per-frame hazard x 60
 *   = 2.4486/s against 1/s = 2.5/s. Fuzzed (tests/fuzz/trials_fuzz.c,
 *   jitter seeds added) under MSVC libFuzzer with ASan: 9,003,538 inputs
 *   in 1201 s, then 4,221,162 in 601 s on the integer version, no crash;
 *   the 2495 corpus files replayed on Linux under ASan and UBSan, no
 *   report. Mutations: tests/mutate/trials.toml, 47 of 47 killed (18 for
 *   jitter). Python binding 156 tests; MEX test_mex.m in MATLAB R2023a and
 *   Octave 10.1. Costs measured (Iris Xe laptop, AC, measurement lock,
 *   examples/trials/bench.c, medians, gcc 16.1 -O2 / MSVC 19.44 /O2): one
 *   ytr_jitter_draw() 66 / 54 ns uniform snapped, 82 / 109 ns
 *   exponential snapped, 32 / 62 ns exponential, 12 / 16 ns choice;
 *   next() + update() with 4 jitters mean 0.16 / 0.20 us, worst 5.2 / 4.8
 *   us. A design without jitters runs as fast as under v0.2.0 (an
 *   interleaved A/B of open(), docs/trials.md).
 *   v0.2.0 STATUS, kept as written: built and run on Windows 11 with MinGW-w64
 *   gcc 16.1 as C11, C99 and C++17 under -Wall -Wextra -Wpedantic -Wshadow
 *   -Werror and with MSVC 19.44 under /W4 /WX, in its default C dialect and
 *   as /std:c++17; on Linux (WSL2) with gcc 11.4 as C11 and C++17 under
 *   -fsanitize=address,undefined -fno-sanitize-recover=all with no
 *   diagnostic; and at YTR_MAX_TRIALS 256. The extra gcc 11.4 warnings
 *   and the clang 11.1 builds of v0.1.1 (below) were not repeated for
 *   v0.2; CI's compilers give that verdict.
 *   v0.1 behavior: tests/adapt/trials_pins.h holds the hashes of 600
 *   v0.1.1 sessions (30 designs x 20 seeds: schedule, history, tallies,
 *   records, format lines, snapshot bytes); all 600 match on gcc, MSVC and
 *   Linux gcc. Both v0.1 examples print what v0.1.1 printed but the
 *   version token.
 *   v0.2 checks (tests/adapt/trials_test_v02.h): a table as the
 *   conditions, its names and texts in the data line; ORDER_LIST played as
 *   given; WITH_REPLACEMENT counts against three weight sets (worst
 *   chi-square at 0.20 of its 99.9 % bound), pairs independent (1.3 on 8
 *   df), and a variate on a running sum never drawing a zero-weight row;
 *   subsets uniform (2-subsets of 5: 13.8 on 9 df); Latin and Williams
 *   squares for n = 1..64, every row, position and adjacent pair; BLOCKED
 *   groups in Williams order for 8 participants, a RANDOM group order
 *   uniform (3.1 on 5 df), LIST, ALTERNATE cycling exactly under a repair;
 *   units: 200 seeds of followed_by with max_run, re-queues and blocks, 0
 *   broken pairs (at most 69 repair moves), 0 units split by a track, the
 *   order uniform over the 6 valid orders (7.4 on 5 df) and a free
 *   trial's slot uniform; balance: exact pair counts on 1600 orders of 8
 *   designs with and without the lead-in, uniform over all balanced orders
 *   of 3 levels (216 seen of 216, 183.0 on 215 df; without self pairs 18
 *   of 18, 19.7 on 17 df); the rules text giving the same schedule as the
 *   C calls, format_rules() pasting back, OpenSesame's lines, 14 error
 *   messages; snapshot format 2 saved and loaded at every third cut of
 *   units, groups and balance sessions, identical to the uninterrupted run;
 *   a digest of v0.2 sessions, identical on gcc (C11, C99, C++17), MSVC (C,
 *   C++17) and Linux gcc.
 *   Fuzzed: tests/fuzz/trials_fuzz.c (rules text, open, a run with
 *   re-queues, breaks, the format functions, a save and a load) under MSVC
 *   libFuzzer with ASan, 9,734,743 inputs in 1351 s, no crash; the 1383
 *   corpus files replayed on Linux under ASan and UBSan, C11 and C++17,
 *   with no report. Mutations: 29 in tests/mutate/trials.toml, all
 *   killed. Python binding: 152 tests,
 *   including the parser against Python's csv module and float(). MEX:
 *   test_mex.m passes in MATLAB R2023a and Octave 10.1.
 *   Costs measured (Iris Xe laptop, AC, measurement lock, 2026-10-07,
 *   examples/trials/bench.c, 21 rounds, medians, gcc 16.1 -O2 / MSVC
 *   19.44 /O2) for open() of a 10,000-row table: file order 0.02 / 0.02
 *   ms, FULL_RANDOM 0.04 / 0.04, CONSTRAINED max_run 3 1.82 / 1.54, units
 *   with max_run 3 5.02 / 7.03, 10,000 draws with replacement 0.73 / 1.33,
 *   a 5,000-row subset 0.14 / 0.14, balance over 8 levels 0.26 / 0.30,
 *   8 BLOCKED groups in Williams order with max_run 3 1.79 / 1.73; the
 *   worst next() + update() of 54,034 in a units session with re-queues
 *   19.0 / 2.2 us. docs/trials.md has the ranges.
 *   What is NOT done (v0.2): no run on macOS or big-endian hardware; the
 *   unit repair is measured to be biased under extra rules (docs/
 *   trials.md), less than a rule-based repair; balance is first order
 *   only and refuses tracks, groups, units and rules on its own factor.
 *   v0.1.1 STATUS, kept as written: built and run on Linux (WSL2) with
 *   gcc 11.4 as C99, C11 and C++17 under -Wall -Wextra -Wpedantic
 *   -Wshadow -Werror, and under -fsanitize=address,undefined
 *   -fno-sanitize-recover=all with no diagnostic; on Windows 11 with MSVC
 *   19.44 under /W4 /WX, in its default C dialect and as /std:c++17. The
 *   header, its test and both
 *   examples also build at -O2 and -O3 under gcc 11.4 with
 *   -Warray-bounds=2 -Wstringop-overflow=4 -Wmaybe-uninitialized
 *   -Waggressive-loop-optimizations -Wnull-dereference added, and under
 *   clang 11.1 as C11, C99 and C++17, all with -Werror; the newer gcc 13,
 *   gcc 16 and clang 18 of CI were not available locally, so their verdict
 *   is CI's. Both examples print byte for
 *   byte the same on the two compilers, and the test prints the same
 *   apart from its timings.
 *   tests/adapt/trials_test.c runs at YTR_MAX_TRIALS 256 and at the
 *   default 4096 and checks: the factorial index round trip; the orders'
 *   properties over 400 seeds (SEQUENTIAL identical to PsychoPy's order,
 *   RANDOM one of each row per repetition, FULL_RANDOM exact counts and a
 *   first slot near uniform, weighted repetitions) and the Fisher-Yates
 *   draw order against a hand trace; every constraint rule on every
 *   repaired order of eight designs x 200 seeds, by a checker written
 *   separately from the header's, including block-clipped windows and
 *   runs; the failure message on impossible designs; track interleaving
 *   at rates 0.5 and 1 and 0 (rejected with conditions), round robin
 *   against a hand trace, weights, and that a finished track is never
 *   picked or asked again; practice and warmup flags, blocks and tallies;
 *   re-queues with and without a gap, and 760 random re-queues under a
 *   no-repeat rule with no unflagged violation in the realized order;
 *   ytr_mark_break() ending a run; tallies against a hand count;
 *   ytr_restore() reproducing history, schedule, tallies, records and
 *   the generator's position bit for bit, with and without tracks;
 *   ytr_save() at 42 cut points (with and without a pending trial)
 *   then ytr_load() into a garbage-filled handle and finishing,
 *   compared bit for bit with the uninterrupted run, snapshot bytes
 *   included; every load refusal; every rejected desc; YTR_ERR_FULL;
 *   the format functions against fixed strings.
 *   Compared with PsychoPy's psychopy.data.TrialHandler and TrialHandlerExt
 *   through the Python binding (tests/compare/compare_trials_psychopy.py),
 *   on the same condition lists over 100 seeds per design. The two use
 *   different generators, so random orders are compared by the properties
 *   each promises: SEQUENTIAL is identical to PsychoPy's order; under RANDOM
 *   every repetition is one block holding each condition once, on both
 *   sides; FULL_RANDOM and weighted FULL_RANDOM give exact counts per
 *   condition on both sides. The weighted SEQUENTIAL orders differ by
 *   design (PsychoPy runs a row's copies back to back, this header cycles
 *   the rows); the script reports that and does not fail on it.
 *   What is NOT done: no run on macOS or big-endian hardware, so the
 *   snapshot's portability is by construction, not by test; no loading
 *   of a snapshot written by another compiler; the constraint repair's
 *   success rate is measured only on the designs in the test, and a
 *   tight design can still need a larger desc.max_swaps. Nothing here is
 *   a timing measurement of a real session. A -DYTR_API=static build
 *   needs a translation unit that calls every function, or
 *   -Wno-unused-function.
 *
 *   ---------------------------------------------------------------------
 *   USAGE
 *   ---------------------------------------------------------------------
 *   Do this:
 *
 *       #define YSP_TRIALS_IMPLEMENTATION
 *
 *   in *one* C or C++ file before including this header to create the
 *   implementation. Every other file just includes the header normally.
 *
 *   Method of constant stimuli, 2 orientations x 5 contrasts, 20
 *   repetitions, random order, never the same orientation four times
 *   running (examples/trials/mocs.c runs this):
 *
 *       #define YSP_TRIALS_IMPLEMENTATION
 *       #include "ysp/trials.h"
 *
 *       uint64_t seed = 20260923u;                  // the caller owns it
 *       ytr_desc d = {                            // unset fields are 0
 *           .factors = { ytr_factor("orientation", 2),
 *                        ytr_factor("contrast", 5) },
 *           .n_factors     = 2,                     // 10 conditions
 *           .reps          = 20,                    // 200 trials
 *           .order         = YTR_ORDER_CONSTRAINED,
 *           .constraints   = { ytr_max_run(0, YTR_ANY_LEVEL, 3) },
 *           .n_constraints = 1,
 *           .rng = ytr_splitmix, .rng_ctx = &seed,
 *       };
 *
 *       static ytr_trials t;                      // 108 KB; not the stack
 *       if (!ytr_open(&t, &d)) { fputs(ytr_error(&t), stderr); return 1; }
 *
 *       ytr_trial_info ti;
 *       while (ytr_next(&t, &ti) >= 0) {
 *           int ori = ytr_level(&t, ti.condition, 0);
 *           int con = ytr_level(&t, ti.condition, 1);
 *           int correct = run_trial(ori, con);      // the caller's business
 *           ytr_update(&t, correct, NULL);
 *       }
 *       for (int c = 0; c < ytr_n_conditions(&t); c++)
 *           printf("%d %.2f\n", c, ytr_proportion(&t, c, 1));
 *
 *   Three interleaved staircases with a catch condition on about one trial
 *   in ten, never two catch trials in a row (examples/trials/interleave.c
 *   runs this):
 *
 *       static double levels[YTR_MAX_TRIALS];     // one record per trial
 *       yst_stair s[3];                           // opened by the caller
 *       ytr_desc d = {
 *           .n_conditions  = 1,                     // the catch trial
 *           .reps          = 24,
 *           .order         = YTR_ORDER_CONSTRAINED,
 *           .constraints   = { ytr_min_gap(YTR_CONDITION, 0, 1) },
 *           .n_constraints = 1,
 *           .tracks = { ytr_track(&s[0], ytr_stair_done),
 *                       ytr_track(&s[1], ytr_stair_done),
 *                       ytr_track(&s[2], ytr_stair_done) },
 *           .n_tracks      = 3,
 *           .track_rate    = 0.9,                   // 9 in 10 while any runs
 *           .rng = ytr_splitmix, .rng_ctx = &seed,
 *           .records       = levels,
 *           .record_size   = sizeof(double),
 *       };
 *       ...
 *       while (ytr_next(&t, &ti) >= 0) {
 *           if (ti.track >= 0) {
 *               double level = yst_next(&s[ti.track]);
 *               int r = run_trial(level);
 *               yst_update(&s[ti.track], level, r);
 *               ytr_update(&t, r, &level);        // the level as the record
 *           } else {
 *               int r = run_catch_trial();
 *               ytr_update(&t, r, NULL);          // a zeroed record
 *           }
 *       }
 *
 *   where ytr_stair_done is the caller's three-line adapter:
 *
 *       static bool ytr_stair_done(void* ctx) {
 *           return yst_done((const yst_stair*)ctx);
 *       }
 *
 *   The schedule there is 24 identical catch trials, so no order of it
 *   keeps them apart; the track trials do. See CONSTRAINTS, AT RUN TIME.
 *
 *   One data row per trial, the session's columns and the caller's
 *   (examples/trials/save_rows.c runs this; DATA FILE has the rules):
 *
 *       typedef struct row {
 *           const char* response;                  // NULL: missed
 *           bool        correct;
 *           int64_t     rt_ns;                     // YTR_MISSING_NS: missed
 *       } row;
 *       const ytr_col cols[] = {
 *           YTR_COL(row, response, YTR_COL_STRING),
 *           YTR_COL(row, correct, YTR_COL_BOOL),
 *           YTR_COL_D(row, rt_ns, YTR_COL_TIME_NS, 6),  // seconds, 6 decimals
 *           YTR_COL_TEXT("participant", "P07"),
 *       };
 *       ytr_data_desc dd = { .path = "P07.csv", .trials = &t,
 *                            .cols = cols, .n_cols = 4 };
 *       static ytr_data w;                        // about 18 KB
 *       if (!ytr_data_open(&w, &dd)) { fputs(ytr_data_error(&w), stderr); return 1; }
 *       while (ytr_next(&t, &ti) >= 0) {
 *           row r = run_trial(&ti);
 *           ytr_update(&t, r.correct, NULL);
 *           ytr_data_write(&w, &r);               // between trials
 *       }
 *       if (!ytr_data_close(&w)) fputs(ytr_data_error(&w), stderr);
 *
 *   ---------------------------------------------------------------------
 *   MODEL
 *   ---------------------------------------------------------------------
 *   A session is a sequence of trials. Each trial comes from one of two
 *   sources: a CONDITION, one row of a table the caller defines, scheduled
 *   in advance and ordered at open; or a TRACK, an adaptive procedure the
 *   caller owns, chosen at run time while it is not done. The header
 *   decides which source and which condition; the caller decides what to
 *   show. It records the outcome and a fixed-size caller record per trial,
 *   tallies outcomes per condition, and hands the history back.
 *   Practice trials run first and warmup trials open the later blocks
 *   (BLOCKS, BREAKS, ...). Every other trial, scheduled or track, is a
 *   MAIN trial; blocks and constraints count main trials only.
 *
 *   CONDITIONS
 *     Either desc.n_conditions rows with no structure (the caller knows
 *     what row 3 means), or a FACTORIAL design: desc.factors[] gives
 *     n_levels per factor and the rows are their Cartesian product, last
 *     factor fastest, so ytr_level(t, condition, factor) returns the
 *     level index of each factor. A factor's name is for the row formatter
 *     only. With factors, n_conditions is 0 or the product.
 *     YTR_MAX_FACTORS (8) factors, and the product must fit
 *     YTR_MAX_CONDITIONS (1024).
 *     desc.reps repetitions of every condition, or desc.cond_reps[] per
 *     condition (a weighted design; PsychoPy's TrialHandlerExt), which
 *     overrides reps when it is not NULL; an entry may be 0, the sum may
 *     not. The practice trials plus the scheduled trials must fit
 *     YTR_MAX_TRIALS (4096; a compile-time size of the handle), and so
 *     must the warmup trials when the session has no tracks, since their
 *     number is then known at open.
 *     open() copies cond_reps and warmup_conditions (at most
 *     YTR_MAX_CONDITIONS entries) into the handle, reading each entry
 *     once, so those two arrays may go away after open(). The other
 *     pointers (records, factor names, track contexts, rng and rng_ctx)
 *     are used after open() returns and must stay valid while the handle
 *     is in use.
 *
 *   ORDER (desc.order)
 *     YTR_ORDER_SEQUENTIAL   repetition-major, as PsychoPy does it: rep
 *                              0 of every row in row order, then rep 1, so
 *                              the rows cycle and the repetitions of one
 *                              row are spread over the session. With
 *                              cond_reps, repetition r holds the rows that
 *                              have more than r repetitions.
 *     YTR_ORDER_RANDOM       each repetition is a block; the rows are
 *                              shuffled within every block independently
 *                              (PsychoPy "random"). Every condition appears
 *                              exactly once per block (with cond_reps, once
 *                              in each block it has a repetition in). These
 *                              repetition blocks are not block_size blocks.
 *     YTR_ORDER_FULL_RANDOM  one shuffle of all reps x rows (PsychoPy
 *                              "fullRandom"). Runs of a condition are
 *                              possible.
 *     YTR_ORDER_CONSTRAINED  FULL_RANDOM followed by repair until every
 *                              constraint holds; see CONSTRAINTS (and
 *                              STRUCTURE and BALANCE for v0.2 rules).
 *     YTR_ORDER_LIST         desc.order_list as given (v0.2).
 *     YTR_ORDER_WITH_REPLACEMENT  desc.draws independent draws by
 *                              desc.weights (v0.2).
 *     ORDER LISTS, DRAWS, SUBSETS and GROUPS give the v0.2 orders.
 *     The shuffle is Fisher-Yates on the caller's generator (RANDOMNESS
 *     gives the draws). Two sessions with the same desc and the same seed
 *     get the same order, and the order is fixed at open, so
 *     ytr_condition_at(t, i) answers for a trial not yet run (a caller
 *     that preloads stimuli needs that). The schedule it indexes is the
 *     practice slots, then the main schedule; warmup trials are drawn at
 *     run time and are not in it. The answer for a slot not yet run holds
 *     unless a re-queue inserts a copy before it or the run-time check
 *     (CONSTRAINTS) moves a trial forward, which a schedule that passed the
 *     repair at open needs only after a re-queue.
 *     The rep of a scheduled trial counts the row's earlier slots in the
 *     main schedule, so under every order it numbers that row's
 *     presentations from 0; a re-queued copy keeps the rep of the trial it
 *     replaces. Practice, warmup and track trials have rep -1.
 *
 *   CONSTRAINTS (desc.constraints[], YTR_ORDER_CONSTRAINED only)
 *     Each constraint names a FACTOR (or YTR_CONDITION for the row
 *     itself), a LEVEL of it (or YTR_ANY_LEVEL: whatever level the trial
 *     at hand has), and a rule:
 *       ytr_max_run(f, l, n)          no more than n consecutive trials
 *                                       with level l of factor f
 *       ytr_max_in_window(f, l, w, n) at most n trials with level l in
 *                                       any w consecutive trials
 *       ytr_min_gap(f, l, g)          at least g other trials between two
 *                                       trials with level l
 *       ytr_no_transition(f, a, b)    level a is never directly followed
 *                                       by level b (a may equal b)
 *       ytr_first_not(f, l)           the session's first main trial
 *                                       does not have level l
 *     YTR_ANY_LEVEL is accepted by the first three. n, w and g are at
 *     least 1. YTR_MAX_CONSTRAINTS (16). A constraint under any other
 *     order is rejected at open.
 *     THE SEQUENCE. Constraints hold on the main sequence in run order.
 *     Practice and warmup trials are not in it. A track trial is, with no
 *     level of any factor: it breaks a run, widens a gap and thins a
 *     window, and never breaks a rule itself. A trial that was re-queued
 *     is in it too: the observer saw it. Unless
 *     desc.constraints_span_blocks is set, the sequence is cut into
 *     SEGMENTS at the start of every block (desc.block_size) and at every
 *     break marked with ytr_mark_break(), and every rule but first_not
 *     looks back only within the segment: a run, a gap and a transition
 *     end at the boundary, and a window that straddles it is clipped to
 *     the trials on this side, so "at most n in any w" counts fewer than w
 *     trials just after a rest. A rest between blocks ends a run as far as
 *     the observer is concerned; set constraints_span_blocks for the
 *     stricter reading, under which marked breaks do not cut either.
 *     AT OPEN. The main schedule is repaired so the rules hold on it
 *     alone, as though no track trial were interleaved. The repair is a
 *     swap search: scan the slots in order for the first whose trial
 *     breaks a rule (each rule looks back from the trial at hand), then
 *     look for a partner to swap it with, starting at a random other slot
 *     and trying up to 64 slots in turn (wrapping around, skipping trials
 *     of the same row); the first swap after which both slots obey every
 *     rule looking back is taken, and when none of the tries gives one,
 *     the swap with the random slot is taken anyway. Rescan from the lower
 *     of the two slots; at most desc.max_swaps swaps (0 = 100000). Without
 *     tracks,
 *     slot i of the main schedule is main trial i, so the segments are the
 *     blocks; with tracks the blocks' slots are not known at open and the
 *     repair counts across them, which is stricter. Adding track trials or
 *     boundaries to a sequence that obeys the rules never breaks one, so
 *     the repaired schedule holds however the run interleaves it.
 *     Without tracks, a repair that runs out of swaps FAILS open() with a
 *     message naming the first constraint still broken, its slot and the
 *     swap count, so an impossible design (two levels, max run 1, unequal
 *     counts) is found at open, not at trial 190. With tracks it does not
 *     fail: the design may rely on track trials to separate its scheduled
 *     trials, as the second USAGE example does, and the run-time check
 *     enforces that. The search is deterministic from the seed and it is
 *     not uniform over the orders that satisfy the rules; no practical
 *     method is.
 *     AT RUN TIME. Before next() hands out a scheduled trial it checks the
 *     trial against the main trials already run. If the trial would break
 *     a rule: while any track is not done, a track trial runs instead;
 *     otherwise the first later slot whose trial fits moves forward to run
 *     now, the rest keeping their order; when none fits, the trial runs
 *     anyway and its history entry carries YTR_FLAG_VIOLATION. That is
 *     the only way a session breaks a rule, and it happens only when the
 *     tracks end before the schedule does, or after a re-queue the rest of
 *     the schedule cannot absorb.
 *     v0.2 adds structural rules (STRUCTURE: UNITS) and transition balance
 *     (BALANCE), with Latin squares for counterbalancing across sessions
 *     and participants (LATIN SQUARES).
 *
 *   TABLES (desc.table; v0.2)
 *     A ysp/table.h table (a conditions file or a trial list, parsed from
 *     CSV or viewed from a pack) gives the conditions: its rows are the
 *     rows, its columns the factors, a factor's levels the column's
 *     distinct values in order of first appearance. desc.factors must then
 *     be empty and desc.n_conditions 0 or the row count, and the rows must
 *     fit YTR_MAX_CONDITIONS (open() says so and names the define). The
 *     table must outlive the handle. A rule names a column by its index
 *     (ytb_col() finds it) and a level by its number (ytb_find()); the
 *     rules text (GRAMMAR) does both by name. ytr_format_header() writes
 *     the column names and ytr_format_row() each cell's text as the file
 *     wrote it (CSV-quoted when needed); ytr_condition_from_levels()
 *     returns the first row with those levels. A fixed trial list played
 *     in file order is a table with SEQUENTIAL order and reps 1; nothing
 *     more is needed.
 *
 *   ORDER LISTS, DRAWS, SUBSETS (v0.2)
 *     YTR_ORDER_LIST plays desc.order_list (n_order_list rows, repeats
 *     allowed) as given: jsPsych's `custom` sample, a Latin-square row, a
 *     list chosen by participant. reps, cond_reps, subset, groups and
 *     constraints are refused with it; the rep of a slot counts the row's
 *     earlier slots as everywhere.
 *     YTR_ORDER_WITH_REPLACEMENT makes desc.draws independent draws, a
 *     row's chance its weight over the total (desc.weights per row, read at
 *     open; NULL for equal weights; finite, not negative, a positive sum).
 *     reps and cond_reps must be 0 / NULL, and constraints are refused: a
 *     repaired sample is not one of independent draws (fix the multiset
 *     with cond_reps and use CONSTRAINED instead).
 *     desc.subset = k keeps k of the rows that have trials, chosen
 *     uniformly, and gives the others no trials, before any order
 *     (jsPsych's without-replacement sample is subset k with FULL_RANDOM
 *     and reps 1). Under SEQUENTIAL the kept rows stay in row order. It is
 *     chosen once at open, not again per repetition. Refused with
 *     structural rules.
 *
 *   GROUPS (desc.groups; v0.2)
 *     The levels of one factor (groups.factor) as an outer order, the
 *     trials of each group ordered by desc.order inside it. A group with
 *     no trials is skipped. Group order: SEQUENTIAL (level order), RANDOM
 *     (one shuffle), LATIN or BALANCED_LATIN (row groups.participant of a
 *     square over all the factor's levels, LATIN SQUARES), or LIST
 *     (groups.list, level numbers; a group with trials missing from it is
 *     an error).
 *     BLOCKED: each group's trials run together and each group is a block:
 *     first_in_block, warmups and constraint segments follow the groups,
 *     and desc.block_size must be 0. CONSTRAINED repairs inside each group
 *     (with constraints_span_blocks the run-time check counts across groups
 *     and the repair does not). WITH_REPLACEMENT makes desc.draws draws in
 *     each group that has a row of positive weight.
 *     ALTERNATE: one trial of each group in group order, cycling (jsPsych's
 *     alternate-groups). Groups of unequal size are refused with their
 *     sizes (jsPsych cuts every group to the smallest). Under CONSTRAINED
 *     the repair swaps only trials of the same group, so the cycle holds.
 *     Groups refuse tracks (a track trial has no group; one handle per
 *     block mixes them), ORDER_LIST and balance; ALTERNATE refuses
 *     structural rules and WITH_REPLACEMENT.
 *     NESTING. Groups are the one level of nesting here: the schedule is a
 *     flat array fixed at open, which preloading (condition_at) and replay
 *     need. Deeper trees, inherited parameters, and nodes decided at run
 *     time (loop and conditional functions) belong to an experiment
 *     definition above this header.
 *
 *   STRUCTURE: UNITS (v0.2; CONSTRAINED only)
 *     Three rules shape the order instead of being repaired:
 *       ytr_followed_by(f, a, b)  every trial with level a of f is
 *                                   directly followed by one with level b
 *       ytr_preceded_by(f, b, a)  every trial with level b is directly
 *                                   preceded by one with level a
 *       ytr_chunk(f)              each run of contiguous rows with one
 *                                   value of f (not "" in a table) is one
 *                                   unit, in row order, every repetition
 *     followed_by pairs each a-trial with a distinct b-trial chosen by a
 *     shuffle (spare b-trials are free; fewer b- than a-trials is an
 *     error); preceded_by the same the other way; with equal counts either
 *     is a prime-target pair. A chunk's rows must have equal repetitions,
 *     and one value in two separate runs is reported as a typo. The rows
 *     that two structural rules touch must not overlap (a longer unit is a
 *     chunk column). The UNITS are shuffled as wholes: with no other rule
 *     the order is uniform over all orders that keep the units, and the
 *     rules cannot be broken. Other rules are checked on the trials and
 *     repaired by moving whole units: the first of up to 64 unit
 *     boundaries, from a random start, where the unit's trials and the slot
 *     that closes the gap obey every rule looking back; else the drawn
 *     boundary. Moves, not swaps: a swap cannot change a sequence whose
 *     one-trial units are all alike (measured, docs/trials.md). This
 *     repair counts runs across block boundaries, since a boundary that
 *     falls inside a unit moves to its end.
 *     AT RUN TIME the trials of a unit run back to back: no track trial,
 *     no draw, no forward move and no block start inside a unit (a
 *     block_size boundary waits for the unit's end, and the blocks after it
 *     keep the block_size grid). A scheduled trial inside a unit that
 *     breaks a rule runs, flagged YTR_FLAG_VIOLATION. The forward move
 *     moves the first later unit all of whose trials fit. A re-queue of any
 *     trial of a unit queues a copy of the whole unit at a unit boundary.
 *     Units are allowed with tracks and with BLOCKED groups (a unit's rows
 *     must then share the group).
 *
 *   BALANCE (v0.2; CONSTRAINED only)
 *     ytr_balance(f): in the main sequence every ordered pair of levels of
 *     f (that have trials) is adjacent equally often, lambda times, a level
 *     followed by itself included; ytr_balance_no_repeat(f) leaves the
 *     self pairs out (no level twice in a row). Every level must have the
 *     same number of trials, n x lambda (n - 1 times lambda without self
 *     pairs) for n levels; open() names the counts that would fit. The
 *     order is drawn uniformly from all such orders: an Euler circuit of
 *     the complete directed graph on the levels, each arc lambda times, by
 *     Kandel, Matias, Unger and Winkler (1996) as Brooks (2012) recommends;
 *     then each level's trials are shuffled into its places. The sequence
 *     starts with a LEAD-IN, one more trial of the start level, so all
 *     n^2 lambda transitions occur (Brooks: "each sequence contains an extra
 *     trial of one condition"). The lead-in is a main trial (the observer
 *     sees it; the rules count it) flagged YTR_FLAG_LEADIN, with rep -1,
 *     left out of the tallies, shown in a `leadin` column of the data lines,
 *     and it cannot be re-queued. ytr_balance_flags(f,
 *     YTR_BALANCE_NO_LEADIN) leaves it out, for a short sequence that
 *     cannot afford the trial: one transition, last to first, is then
 *     missing. Other rules on other factors are repaired by swaps between
 *     trials of the same level of f, which keep every transition; a rule on
 *     f itself (or any rule when f is YTR_CONDITION) is refused, because
 *     no exact sampler has both properties and rejection fails as orders
 *     grow (docs/trials.md has the numbers). Balance refuses tracks
 *     (track trials cut transitions), groups and structural rules. It
 *     counts transitions across block boundaries. A re-queue adds
 *     transitions; the realized order is then not exactly balanced, and the
 *     history shows the re-queued trial.
 *
 *   LATIN SQUARES (ytr_latin; v0.2)
 *     ytr_latin(n, row, balanced, out) fills out[n] with row `row` of an
 *     n x n square and returns the number of rows of the design. Cyclic:
 *     out[j] = (row + j) mod n, n rows, every item once in every position
 *     over the rows. Balanced (Williams 1949): first row 0, 1, n-1, 2, n-2,
 *     3, ...; row r adds r mod n to each entry; for even n the n rows have
 *     every ordered pair adjacent exactly once; for odd n the design has 2n
 *     rows, rows n..2n-1 the reverses of rows 0..n-1, and every ordered
 *     pair adjacent exactly twice. row is taken modulo the rows, so a
 *     participant number goes in directly. Uses: an order list for one
 *     sequence of conditions per participant; a group order (LATIN,
 *     BALANCED_LATIN) for counterbalanced blocks; session k of participant p
 *     runs item out[k] of row p.
 *
 *   GRAMMAR (ytr_rules; v0.2)
 *     Rules text sets the order and constraint fields of a desc, a
 *     statement per line, one statement per C call or desc field, columns
 *     and levels by name. ytr_format_rules() writes a session's settings
 *     the same way, so a logged session pastes back in.
 *       # a comment                      blank lines and comments anywhere
 *       order constrained
 *       reps 10
 *       where list=@participant          keep the rows of one list
 *       max_run target 3                 any level of target
 *       max_in_window target=catch 6 1   level by its text (or value)
 *       followed_by cue valid probe
 *     A line is words separated by blanks; `#` outside quotes starts a
 *     comment; a token may be quoted ("new york", "" for a quote). A
 *     SELECTOR is COLUMN (any level) or COLUMN=VALUE; @row is the row itself
 *     (YTR_CONDITION), with row numbers as values; @participant as a value
 *     is level number (participant mod the level count). A value is the
 *     level's text for a STRING column, compared by value for a numeric one
 *     ("0.50" finds 0.5), and a level number for a factor with no table.
 *     A count is decimal digits. Statements and what they set:
 *       order sequential|random|full_random|constrained|list|with_replacement
 *                                        desc.order
 *       order latin|balanced_latin       ORDER_LIST, a row of the square over
 *                                        the rows, row = participant
 *       reps N                           desc.reps
 *       cond_reps N N ...                desc.cond_reps (one per row)
 *       weight COLUMN                    desc.cond_reps from an integer
 *                                        column (OpenSesame's weight)
 *       where SELECTOR                   rows that do not match get no
 *                                        trials (repeatable, all must hold)
 *       subset K                         desc.subset
 *       draws N [weights=COLUMN]         desc.draws, desc.weights
 *       list ROW ROW ...                 desc.order_list, appended per line
 *       groups COLUMN blocked|alternate [sequential|random|latin|
 *              balanced_latin|list VALUE ...]   desc.groups
 *       block_size N, warmup N, requeue_gap N, max_swaps N, span_blocks
 *       practice N [from SELECTOR]       desc.n_practice and the rows it
 *                                        draws from
 *       warmup_conditions ROW ROW ...    desc.warmup_conditions
 *       max_run SELECTOR N               ytr_max_run()
 *       max_in_window SELECTOR W N       ytr_max_in_window()
 *       min_gap SELECTOR G               ytr_min_gap()
 *       no_transition COLUMN A B         ytr_no_transition()
 *       first_not COLUMN=VALUE           ytr_first_not()
 *       followed_by COLUMN A B           ytr_followed_by()
 *       preceded_by COLUMN B A           ytr_preceded_by()
 *       chunk COLUMN                     ytr_chunk()
 *       balance COLUMN [no_repeat] [no_leadin]   ytr_balance_flags()
 *       constrain COLUMN [maxrep=N] [mindist=D]  OpenSesame: max_run N, and
 *                                        min_gap D - 1 (mindist counts
 *                                        rows: 2 is no immediate repeat)
 *       shuffle                          OpenSesame: order full_random
 *       jitter NAME uniform LO HI [rate=NUM[/DEN]]          desc.jitters[]
 *       jitter NAME choice V V ... [rate=NUM[/DEN]]         (v0.2.1); LO, HI
 *       jitter NAME exponential LO HI SCALE [rate=NUM[/DEN]] and SCALE are
 *                                        seconds or a numeric column
 *     A statement that sets one field may come once (a second names the
 *     first's line); where, list and the rules accumulate (rules are
 *     appended after any the desc holds). The participant number is
 *     session data: it comes from ytr_rules_desc, never from the text.
 *     ERRORS: ytr_rules() returns 0 or the line of the first error, and
 *     writes "ysp_trials: rules line L, col C: what" with what it knows:
 *     the expected form, the known columns, up to 8 levels, the first line
 *     of a repeated statement, "did you mean" for a near verb, the
 *     statement here for another tool's word (maxrep: max_run; mindist:
 *     min_gap), and OpenSesame operations it does not take (slice, sort,
 *     sortby, reverse, roll, shuffle_horiz, fullfactorial, setcycle). Whether
 *     the design can be met is open()'s question, and open()'s messages name
 *     the constraint, which ytr_format_rules() prints by name.
 *     BOUNDS AND MEMORY: 4096 bytes per line, 64 arguments per line, 100000
 *     lines. Nothing allocates: the arrays the rules make (cond_reps,
 *     order_list, weights, the practice list, the group list, a jitter's
 *     name and values) are carved from rules_desc.arena (at most 4 x
 *     YTR_MAX_TRIALS + 32 bytes per row + 300 bytes per jitter; a short
 *     arena is an error naming the bytes needed). open() and
 *     load() copy or read them, so the arena may go away after open().
 *
 *   JITTER (desc.jitters[], desc.n_jitters; v0.2.1)
 *     A jitter is a duration drawn for every trial: a foreperiod, an
 *     inter-trial interval, an SOA. The header draws and logs it; the
 *     caller waits it. The drawn duration is a whole number of
 *     nanoseconds, .ns (int64), for a clock; .s is the same duration in
 *     seconds, the correctly rounded .ns / 1e9; .frames is the whole frame
 *     count when the jitter snaps, else -1. lo, hi, s and the values are
 *     given in seconds and taken to the nearest nanosecond.
 *       ytr_uniform(name, lo, hi)          uniform over the nanoseconds
 *                                            of [lo, hi]
 *       ytr_choice(name, values, n)        one of n values (at most
 *                                            YTR_MAX_JITTER_VALUES, 32),
 *                                            each 1/n likely; repeat a value
 *                                            to weight it
 *       ytr_exponential(name, lo, hi, s)   lo + an exponential of mean s,
 *                                            truncated to [lo, hi)
 *       ytr_frames(j, num, den)            j snapped to whole frames of
 *                                            num/den Hz (ysp/timeline.h's
 *                                            rates; den 0 means 1)
 *     0 <= lo <= hi <= YTR_JITTER_MAX_S (1e6 s), 1 ns <= s <= 1e6 s,
 *     values in [0, 1e6] s. The name heads the jitter's data-line column:
 *     1 to 31 of [A-Za-z0-9_], not starting with a digit, unique in the
 *     session.
 *     THE NON-AGING FOREPERIOD. An exponential has a constant hazard: the
 *     chance that the target comes in the next instant, given that it has
 *     not come yet, is 1/s whatever the time already waited, so waiting
 *     tells the observer nothing and expectancy stays flat. TRUNCATION at
 *     hi bends that: the density is e^-(t-lo)/s / (s (1 - e^-(hi-lo)/s)) on
 *     [lo, hi], the hazard at t is (1/s) / (1 - e^-(hi-t)/s), so it is
 *     within e^-3 = 5 % of flat until 3 s before hi and grows without
 *     bound at hi, and the mean is lo + s - (hi-lo) e^-(hi-lo)/s /
 *     (1 - e^-(hi-lo)/s). The draws the truncation removes, e^-(hi-lo)/s
 *     of the untruncated ones (2.4 % for lo 0.5, hi 2.0, s 0.4), are
 *     spread over [lo, hi) in proportion to the density, not piled at hi
 *     (THE DRAW). Choose hi - lo of 3 to 4 s or more.
 *     FRAMES. A snapped jitter is a whole number of frames k, its
 *     duration k den / num s, which .ns rounds to the nearest nanosecond
 *     (ties up). The frames inside [lo, hi] are those whose .ns lies in
 *     [lo, hi], all in integers, so 0.07 s at 100 Hz is frame 7 though 0.07
 *     x 100 is 7.000000000000001 in binary. UNIFORM: each of those frames
 *     equally likely. CHOICE: each value moved to its nearest frame (ties
 *     up). EXPONENTIAL: a geometric over those frames, P(k) proportional
 *     to q^(k - first),
 *     q = e^-(den / (num s)): the per-frame hazard is exactly
 *     1 - q, the chance an exponential of mean s ends within one frame,
 *     until the truncation term 1 / (1 - q^(frames left)) takes over as
 *     above. As a rate per second it is (1 - q) num/den, slightly below
 *     1/s: 2.4486/s against 2.5/s at 60 Hz with s = 0.4 (2.1 % low). The
 *     test measures it (STATUS). When no whole frame lies in [lo, hi],
 *     open() and ytr_jitter_check() refuse and name the rate and the
 *     interval.
 *     PER CONDITION. With desc.table, lo_column, hi_column and (for
 *     EXPONENTIAL) scale_column name numeric columns whose cells replace
 *     lo, hi and s for a trial of that row; open() checks every row and
 *     names the first bad one. A track trial has no row and uses the
 *     jitter's own numbers, which open() then checks too.
 *     THE DRAW. Each jitter takes one raw draw u per new trial
 *     (RANDOMNESS gives the order), so the generator's position after a
 *     trial does not depend on the values drawn, and maps u53 = floor(u
 *     2^53) (u clamped to [0, 1)) in integer arithmetic only: no libm call
 *     and no floating-point operation that a C library, a compiler or a
 *     flag (-Ofast, /fp:fast, FMA contraction) could round differently, so
 *     a draw is the same nanosecond on every platform, and a session
 *     replays exactly in another build (the Python binding regenerates a
 *     Windows session). UNIFORM: lo + floor(u53 (hi - lo + 1) / 2^53) ns.
 *     CHOICE: value floor(u53 n / 2^53). EXPONENTIAL: E = s (-ln(1 - u)),
 *     the inverse CDF of the untruncated exponential, in nanoseconds
 *     rounded half up, then lo + (E mod (hi - lo)): the exponential is
 *     memoryless, so E folded into the interval is exactly the truncated
 *     exponential, with no exp() needed (hi - lo = 0 gives lo). Snapped,
 *     the same with E counted in whole frames, G = floor(-ln(1 - u) x the
 *     scale in frames, s num / (den 1e9) held to 62 bits), folded into the
 *     frames inside. -ln(1 - u) is a Q58 fixed-point logarithm (a 128-entry
 *     table of reciprocals and their logarithms, from mpmath, and a
 *     degree-8 integer series): within 2^-57 of the exact value by
 *     analysis, 0.71 x 2^-58 at worst over 1,000,000 values checked against
 *     mpmath (tests/compare/trials_jitter_ref.py), so E is within 0.5 ns +
 *     s x 7e-18 of s (-ln(1 - u)) for the u53 drawn.
 *     ytr_jitter(t, i, j) returns trial i's draw any time after next()
 *     handed trial i out; ytr_jitter_index() finds j by name. The data
 *     line (ytr_format_row) ends with one column per jitter, named by
 *     the jitter, the seconds as the exact decimal of .ns
 *     (1.184516667); ytr_format_meta() and ytr_format_rules() write
 *     the jitters' settings.
 *     CALL LEVEL. ytr_jitter_draw(&j, rng, ctx) calls rng once and maps
 *     the variate; ytr_jitter_map(&j, u) maps a variate the caller has;
 *     ytr_jitter_check() says why a desc is refused (columns need a
 *     session). They touch no session: a draw made with the session's
 *     generator between next() and update() moves every later draw, and
 *     restore() would not repeat it. Use desc.jitters (replayed and
 *     logged), or a generator of the caller's own that the caller logs.
 *
 *   TRACKS (desc.tracks[], desc.n_tracks, desc.interleave, desc.track_rate)
 *     A track is {ctx, is_done(ctx), weight}. While a track is not done and
 *     a scheduled trial is left, next() runs a track trial with
 *     probability desc.track_rate and otherwise the next scheduled trial.
 *     track_rate defaults to 1 with no conditions and to 0 with no tracks;
 *     with both it is required, in (0, 1], and 1 runs the tracks to their
 *     end before the schedule. When the schedule is exhausted the tracks
 *     run alone; when every track is done the schedule runs alone. A
 *     scheduled trial that would break a constraint gives way to a track
 *     trial, with no draw (CONSTRAINTS). Among tracks:
 *     YTR_INTERLEAVE_RANDOM draws by weight among those not done (weight
 *     0 means 1), YTR_INTERLEAVE_ROUND_ROBIN cycles through them in index
 *     order, skipping any that are done. Once is_done() has returned true
 *     for a track it is never called on it or picked again. next() calls
 *     is_done() once on each track not yet done each time it chooses a
 *     main or warmup trial, never more than once per track per call, and
 *     not when it hands out a practice trial or returns the pending trial
 *     again; ytr_done() calls it too. The header never calls anything
 *     else on a track: the caller asks its own handle for the level, as
 *     the USAGE example shows. YTR_MAX_TRACKS (16).
 *
 *   BLOCKS, BREAKS, PRACTICE, WARMUP, CATCH TRIALS, RE-QUEUES
 *     desc.block_size splits the main sequence into blocks of that many
 *     main trials (0 = one block); a trial later re-queued counts, since
 *     the observer spent the time. Practice trials are block -1 and the
 *     main trials blocks 0, 1, ...; ytr_trial_info.block and
 *     .first_in_block tell the caller where a rest screen goes.
 *     first_in_block is set on the first practice trial, on the first main
 *     trial, and on the first trial of every later block, which is its
 *     first warmup trial when there are warmups; that trial also has
 *     after_break. The header has no clock: a break by elapsed time is the
 *     caller's check between trials, and ytr_mark_break(t) records that
 *     one happened. Called between trials it flags the next trial handed
 *     out; called with a trial pending it flags that one; either way a
 *     constraint segment starts at the next main trial (the pending one,
 *     when that is a main trial). It does not start a block or run warmup
 *     trials, and it changes nothing in the order: a boundary only relaxes
 *     a rule the schedule already meets. So a break taken on the caller's
 *     own schedule leaves the same flag in the history as a scheduled one.
 *     desc.n_practice trials run before the schedule, drawn at open at
 *     random from desc.warmup_conditions[] (any condition when that list is
 *     empty), or cycled through that list in order under SEQUENTIAL;
 *     flagged practice, excluded from the tallies and the constraints.
 *     desc.n_warmup trials run at the start of every block after the
 *     first, when a main trial is left to follow them, drawn at run time
 *     the same way (under SEQUENTIAL the cycle restarts in each block) and
 *     flagged warmup: a reintroduction after a rest. n_warmup needs
 *     block_size. Both are condition trials, never track trials: the
 *     header does not know what an easy level is, so a warmup for a
 *     staircase is the caller's, which sees the flag, shows what it likes,
 *     and decides whether the track hears about it.
 *     A catch trial is a condition like any other, placed by the
 *     constraints; the second USAGE example is that.
 *     ytr_requeue(t), called INSTEAD of ytr_update() for a scheduled
 *     trial (not a practice, warmup or track trial: YTR_ERR_ARG), records
 *     YTR_REQUEUE as its outcome and inserts a copy (same row, same rep,
 *     flagged requeued) into the schedule: at the end when
 *     desc.requeue_gap is 0; otherwise at a random slot with at least
 *     requeue_gap scheduled trials before it, or at the end when fewer are
 *     left. For a missed response or a fixation break. The copy is tallied
 *     when it runs. When the history could not hold it (trials handed out
 *     + slots left + 1 > YTR_MAX_TRIALS) nothing is inserted, the trial
 *     is recorded as YTR_INVALID, and requeue returns YTR_ERR_FULL. The
 *     copy is placed without a constraint check; the run-time check keeps
 *     the realized order within the rules where it can.
 *
 *   THE LOOP
 *     ytr_next(t, &info)        the next trial: its index, or YTR_DONE
 *     ytr_update(t, outcome, rec) what happened, plus the caller's record
 *     ytr_done(t)               nothing pending or scheduled, tracks done
 *     ytr_requeue(t)            do this one again later
 *   next() without an update() in between returns the same trial again
 *   (info may be NULL); update() or requeue() without a pending trial is
 *   YTR_ERR_ORDER. `outcome` is any int >= 0 the caller chooses (1 =
 *   correct, a key code, a category), or YTR_INVALID (-1) for a trial
 *   that produced no usable response, excluded from the tallies. Lower
 *   values are YTR_ERR_ARG: YTR_REQUEUE (-2) is what requeue()
 *   records. `rec` copies desc.record_size bytes into the trial's slot of
 *   desc.records; NULL zero-fills the slot; with no records buffer (or
 *   record_size 0) it is ignored.
 *
 *   TALLIES AND HISTORY
 *     ytr_count(t, c, outcome) and ytr_proportion(t, c, outcome) over
 *     the completed, valid (outcome >= 0), non-practice, non-warmup trials
 *     of condition c; ytr_n_valid(t, c) is how many. count() also
 *     counts the YTR_INVALID or YTR_REQUEUE trials of c when asked;
 *     proportion() is NaN for those and when n_valid is 0. That is the
 *     method-of-constant-stimuli estimate; a psychometric fit is an
 *     offline job on the history, as everywhere in this collection.
 *     ytr_history(t, &n) is the trial array, a pending trial included
 *     (without YTR_FLAG_DONE, outcome YTR_INVALID); ytr_record(t, i)
 *     the caller's bytes for trial i. ytr_format_row(t, i, buf, cap)
 *     writes one newline-terminated CSV line:
 *       index,block,rep,condition,track,practice,warmup,requeued,
 *       after_break,outcome
 *     then one column per factor level (empty for a track trial); the
 *     outcome is empty while the trial is pending. ytr_format_header()
 *     writes the matching header, with the factor names (factor<i> when a
 *     name is NULL, quoted when it holds a comma, a quote or a line break).
 *     No file is touched.
 *
 *   SAVING, REPLAY AND RESUME
 *     The header's own state is small and scalar: the desc's numbers, the
 *     schedule, the history (outcomes and flags), the tallies. Three ways
 *     out, none of them a file:
 *       ytr_format_header / ytr_format_row   CSV lines for analysis
 *       ytr_format_meta                        one line of the desc's
 *                                                numbers and the counts, as
 *                                                key=value pairs, to head a
 *                                                data file
 *       ytr_save / ytr_load                  a versioned byte snapshot
 *                                                of the whole state, for
 *                                                resuming after a crash
 *     The data writer (DATA FILE, v0.3) puts the first two in a file with
 *     the caller's columns.
 *     ytr_save() writes ytr_save_size(t) bytes; ytr_load() rebuilds a
 *     handle from them plus a desc that re-supplies what a snapshot cannot
 *     carry (the pointers: rng and its state, records, cond_reps, the
 *     warmup list, factor names, tracks), checks that the desc's numbers
 *     match the saved ones, and continues at the pending trial when one
 *     was pending. The generator's state is the caller's: save it beside
 *     the snapshot (for ytr_splitmix, the uint64_t) and put it back
 *     before the next draw. ytr_load() draws nothing and calls no track.
 *     SNAPSHOT LAYOUT, format 1. Every integer is little-endian two's
 *     complement, written and read byte by byte, so a snapshot moves
 *     between compilers and platforms; an f64 is the IEEE 754 bit pattern
 *     as a u64. Defaults are written resolved (max_swaps 0 as 100000,
 *     weight 0 as 1, track_rate as used).
 *       magic     4 bytes "PSTR", then u32 format (1)
 *       desc      i32 n_conditions, i32 n_factors, i32 n_levels per
 *                 factor, i32 reps, u8 has_cond_reps and then i32 per
 *                 condition when set, i32 order, i32 n_constraints and
 *                 per constraint i32 rule, factor, level, level2, n,
 *                 window; i32 max_swaps, i32 n_tracks, f64 weight per
 *                 track, i32 interleave, f64 track_rate, i32 block_size,
 *                 u8 constraints_span_blocks, i32 n_practice, i32
 *                 n_warmup, i32 n_warmup_conditions and i32 per entry,
 *                 i32 requeue_gap, u64 record_size
 *       state     i32 n_scheduled, n_run, n_done, current, schedule_pos,
 *                 rr_next; u32 track-done mask; i32 break_pending, n_main,
 *                 seg_start, warm_block, warm_done, swaps
 *       schedule  n_scheduled x i16 condition, then n_scheduled x i16 rep,
 *                 then n_scheduled x u8 flags
 *       main      n_main x i16 condition (-1 for a track trial)
 *       history   n_run x (i16 condition, i8 track, u8 flags, i16 rep,
 *                 i16 block, i32 outcome)
 *       tallies   n_conditions x i32 valid, then n_conditions x i32
 *                 outcome-1 count
 *       records   n_run x record_size bytes, when record_size > 0
 *     SNAPSHOT LAYOUT, format 2 (v0.2), for a desc with any v0.2 field:
 *     format 1 with, at the end of the desc section, u64 table hash (0
 *     without a table), i32 conditions, i32 factors, i32 n_order_list, i32
 *     draws, i32 subset, i32 groups.mode, groups.factor, groups.order,
 *     groups.participant, groups.n_list, u64 hash of the order list,
 *     weights and group list (which the handle does not keep), and at the
 *     end of the state section i32 blk, i32 blk_next, u8 units, u8
 *     has_leadin. schedule_flags then also carry the unit, block-start and
 *     lead-in marks of each slot.
 *     SNAPSHOT LAYOUT, format 3 (v0.2.1), for a desc with jitters: format
 *     2 (its desc and state sections whether or not a v0.2 field is used)
 *     with, at the end of the desc section, i32 n_jitters and per jitter u8
 *     name length and the name's bytes, i32 dist, f64 lo, hi, scale, i32
 *     n_values and f64 per value, i32 rate_num, rate_den, i32 lo, hi and
 *     scale column (-1 for none); and after the records, n_run x n_jitters
 *     i64 draws in trial order (nanoseconds, or the frame count of a
 *     snapped jitter).
 *     ytr_load() compares the desc section with the desc it is given and
 *     names the first field that differs, then range-checks every number
 *     it reads, so a wrong, truncated or corrupt snapshot fails the load
 *     with a message instead of indexing outside the handle. When
 *     record_size > 0 the records are copied into desc.records.
 *     Replay is the other route: the order is a function of desc and the
 *     generator's state at open, and every run-time draw consumes the
 *     generator in a fixed order, so ytr_restore(t, outcomes, records,
 *     n) after a fresh open() with the same desc and seed rebuilds the
 *     same run: for each trial it calls next(), then requeue() when the
 *     outcome is YTR_REQUEUE and update() otherwise, so the outcomes in
 *     ytr_history() feed it directly. Two limits: a break marked with
 *     ytr_mark_break() is not in the outcomes (drive the loop by hand
 *     and mark it again, or use a snapshot); and next() asks the tracks
 *     is_done(), so with tracks each must answer at every trial as it did
 *     in the original run, which a track restored to its final state does
 *     not. For a session with tracks, drive the loop by hand, feeding each
 *     track its own recorded trial, or use a snapshot. Track state is the
 *     caller's either way; the method headers replay from their own
 *     histories.
 *     The caller's per-trial data is deliberately NOT the header's
 *     problem beyond desc.record_size bytes per trial. Large or variable
 *     data (an eye trace, an EEG epoch, audio) belongs in its own
 *     container under the trial index, and the fixed record holds a
 *     reference to it if one is needed. The sequencer's table stays narrow
 *     and joins on trial index.
 *
 *   DATA FILE (v0.3)
 *     ytr_data writes one CSV row per trial: the session's columns
 *     (desc.trials; the columns of ytr_format_row()) and then the
 *     caller's (desc.cols). Without a session it writes the caller's
 *     columns alone, for a staircase-only experiment or any other table.
 *     COLUMNS. Declared once, at open; open() copies them, so the array may
 *     go away. A name is [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes, unique
 *     among all the file's columns (a session's included), and the file
 *     has at most YTR_DATA_MAX_COLS (64): ysp/table.h's header rules, so
 *     the file reads back. A column gets its value one of three ways:
 *       YTR_COL(T, field, type)     from the field of the struct of type T
 *                                     that ytr_data_write() is given; the
 *                                     name is the field's. open() checks
 *                                     the field's size against the type.
 *       YTR_COL_SET(name, type)     from ytr_data_set_<type>(w, col, v)
 *                                     before the row (PsychoPy's addData;
 *                                     col is the index in desc.cols, or
 *                                     ytr_data_col(w, name)); empty in a
 *                                     row where it was not set. A string
 *                                     set this way must stay valid until
 *                                     the row is written.
 *       YTR_COL_TEXT(name, text)    the same text in every row (participant,
 *                                     session, condition file), copied at
 *                                     open; at most YTR_DATA_POOL / 3 bytes
 *                                     in all.
 *     YTR_COL_D(T, field, type, decimals) is YTR_COL with decimals.
 *     TYPES AND TEXT. Every field is written without a locale ('.' for the
 *     decimal point):
 *       INT, INT64  decimal; YTR_MISSING_INT (INT_MIN) or YTR_MISSING_I64
 *                   (INT64_MIN) is missing
 *       DOUBLE      decimals 0: the fewest of 15 to 17 significant digits
 *                   that ysp/table.h reads back as the same double (%g
 *                   form, so 1e-07); 1..17: that many fixed decimals (%f).
 *                   NaN and the infinities are missing: ysp/table.h reads
 *                   no inf or nan.
 *       BOOL        1 or 0
 *       STRING      const char*, UTF-8; NULL is missing
 *       CHARS       char[size], to the first NUL or all size bytes
 *       TIME_NS     int64_t nanoseconds as seconds with decimals 1..9 (0 =
 *                   9, the exact value): 1184516667 is 1.184516667, and at
 *                   3 decimals 1.185. Integer arithmetic, rounded half away
 *                   from zero; a value that rounds to zero has no sign.
 *                   YTR_MISSING_NS (INT64_MIN) is missing. Data files are in
 *                   seconds; the C side stays int64 ns.
 *     A missing value is an empty field. ysp/table.h then needs
 *     allow_empty for a numeric column (the cell reads as NaN). An empty
 *     string and a missing one are the same empty field, and a row whose
 *     every field is empty is skipped by ysp/table.h's reader.
 *     A text field is quoted when it holds a comma, a quote, a CR or an LF,
 *     with quotes doubled (RFC 4180). A byte that is not part of valid
 *     UTF-8 (ysp/table.h's rules: no overlong forms, no surrogates) is
 *     written as U+FFFD, so one bad byte cannot make the file unreadable.
 *     Lines end in LF; there is no BOM and no comment line. ysp/table.h
 *     refuses a field over YTB_MAX_FIELD (4096) bytes; the writer writes it
 *     whole anyway.
 *     THE SESSION'S COLUMNS. With desc.trials (open at ytr_data_open()),
 *     each row starts with ytr_format_row()'s fields for the trial, and
 *     the header with ytr_format_header()'s names, which must follow the
 *     name rule: a factor named "spatial freq" is refused at open, by name.
 *     ytr_data_write() writes the last completed trial. Call it after
 *     ytr_update() or ytr_requeue() and before the next ytr_next(): with a
 *     trial pending it returns YTR_ERR_ORDER, because the caller's values
 *     belong to the pending trial and the session's to the one before; a
 *     second write for the same trial is YTR_ERR_ORDER too. A trial with no
 *     write has no row (the index column shows the gap). A writer opened
 *     mid-session writes the trials completed after it opens, so after a
 *     ytr_load() resume a new file continues where the snapshot left off.
 *     WHEN BYTES MOVE. A row is built in the handle's buffer (YTR_DATA_BUF,
 *     8192; a longer row goes out in pieces) and handed to the OS in one
 *     write before ytr_data_write() returns: the file has no stdio buffer,
 *     so a crash, an abort() or the screen header's panic exit keeps every
 *     row written. YTR_FLUSH_SYNC also waits for the disk (_commit() on
 *     Windows, fsync() elsewhere), so a power cut keeps them too. Write
 *     between trials: a row costs microseconds with YTR_FLUSH_ROW and
 *     milliseconds with YTR_FLUSH_SYNC (STATUS has the numbers). With a
 *     write function (desc.write instead of desc.path) each row is one
 *     call, and the function decides when bytes are safe.
 *     THE FILE. desc.path is UTF-8 (on Windows converted for _wfsopen),
 *     created or truncated. On Windows other programs may read it while it
 *     is open (a monitor), not write it. A failed write sets ytr_data_error(),
 *     returns YTR_ERR_IO, and every later write returns YTR_ERR_IO without
 *     writing, so the file never has a row after a hole. close() returns
 *     false when any write failed.
 *     THE SIDECAR (desc.meta_path). Lines of key=value written at open:
 *     "ysp_trials=<version> data=<path>", the caller's desc.meta text
 *     as given (participant, rig, seed, the rig profile's hash), and
 *     ytr_format_meta()'s line when there is a session. close() appends
 *     "rows=N closed=1"; a sidecar without it belongs to a session that
 *     did not close. A sidecar, not comment lines in the CSV, because
 *     ysp/table.h, pandas' defaults and R's read.csv take no comments.
 *
 *   RANDOMNESS
 *     desc.rng(ctx) returns a uniform variate in [0, 1). Required for every
 *     order but SEQUENTIAL, for random interleaving of more than one
 *     track, for a track_rate below 1 with both conditions and tracks, for
 *     practice and warmup draws except under SEQUENTIAL, and for a
 *     requeue_gap; open() says which if it is missing. The header owns no
 *     generator and seeds nothing. ytr_splitmix() is a documented
 *     convenience: a splitmix64 step on a uint64_t the CALLER owns, so
 *     `d.rng = ytr_splitmix; d.rng_ctx = &seed;` is the whole setup and
 *     the seed is the caller's to log.
 *     DRAW ORDER. This is a contract: replay and resume depend on it. An
 *     "index draw below m" is floor(u * m) clamped to [0, m - 1], so a
 *     variate outside [0, 1) is clamped, not rejected. A "raw draw" is u
 *     itself.
 *     In ytr_open(), in this order:
 *       1. practice: one index draw per practice trial, in trial order,
 *          below the warmup list's length (or n_conditions). None under
 *          SEQUENTIAL.
 *       2. order: the main schedule starts in SEQUENTIAL order. RANDOM
 *          shuffles each repetition block in turn; FULL_RANDOM and
 *          CONSTRAINED shuffle the whole main schedule once. A shuffle of
 *          m items is Fisher-Yates from the top: for i = m-1 down to 1,
 *          j = an index draw below i + 1, swap items i and j (m - 1
 *          draws).
 *       3. repair (CONSTRAINED): one index draw per swap, s below n - 1
 *          for a main schedule of n slots; the partner search for the
 *          trial in slot k starts at slot (k + 1 + s) mod n.
 *     In ytr_next(), when it chooses a new trial:
 *       4. a practice trial: nothing.
 *       5. a warmup trial: one index draw into the list. None under
 *          SEQUENTIAL.
 *       6. a main trial: when a scheduled trial and a live track are both
 *          available, the scheduled trial breaks no rule, and track_rate
 *          is below 1: one raw draw u, a track trial when u < track_rate.
 *          Then, for a track trial under RANDOM interleave with more than
 *          one live track: one raw draw u, and the track is the first, in
 *          index order, whose running sum of live weights exceeds u times
 *          the live weights' total.
 *     In ytr_requeue() with requeue_gap > 0:
 *       7. one index draw for the slot, even when fewer than requeue_gap
 *          slots are left and the copy goes at the end.
 *     v0.2. A desc with none of the v0.2 fields draws exactly as above. In
 *     open(), after step 1 and in place of steps 2 and 3:
 *       subset: an index draw below m - i for i = 0..k-1, a partial
 *          Fisher-Yates over the m rows that have trials, in row order.
 *       groups RANDOM: one shuffle of the group levels that have trials,
 *          in level order.
 *       per part (the whole main schedule, or each BLOCKED group in run
 *          order, or each ALTERNATE group in level order):
 *          SEQUENTIAL nothing; RANDOM a shuffle per repetition block;
 *          FULL_RANDOM one shuffle; WITH_REPLACEMENT one raw draw u per
 *          trial, the row the first in row order whose running sum of
 *          weights exceeds u times the total; CONSTRAINED with structural
 *          rules: per rule in constraint order, one shuffle of the partner
 *          trials (followed_by: the second level's; preceded_by: the
 *          first level's), then one shuffle of the units; CONSTRAINED
 *          without: one shuffle. Then the repair: one index draw per swap
 *          (below the part's length minus 1) or per unit move (below the
 *          number of units minus 1).
 *       balance: the start level (an index draw below the level count),
 *          the backward walk (an index draw below the number of
 *          predecessors per step until every level is seen), each level's
 *          arc order in level order (a shuffle of its arcs but the tree
 *          arc), each level's trials in level order (a shuffle), the
 *          lead-in's trial (an index draw below the level's trial count);
 *          then the repair if there are other rules.
 *     In ytr_next(): nothing inside a unit. In ytr_requeue() with units,
 *     groups or a lead-in and requeue_gap > 0: one index draw among the
 *     unit boundaries from the gap (and the unit's end) to the end of the
 *     schedule, or of the BLOCKED group's block.
 *     v0.2.1. A desc with no jitters draws exactly as above. With jitters,
 *     every new trial that next() hands out (practice, warmup, main,
 *     track, a re-queued copy, a lead-in) takes, after all of the draws
 *     above for it, one raw draw per jitter in jitter order. open() draws
 *     none, so the schedule is the one without jitters; next() returning
 *     the pending trial again draws none.
 *     Nothing else draws: not update(), mark_break(), load(), done(), the
 *     format functions or the tallies.
 *
 *   ---------------------------------------------------------------------
 *   RETURN VALUES AND ERRORS
 *   ---------------------------------------------------------------------
 *   ytr_open() and ytr_load() return bool and fill ytr_error().
 *   Everything else returns a value or a code and never touches the
 *   message. ytr_next() returns the trial index (>= 0) or YTR_DONE
 *   (-1) or an error below -1; ytr_strerror() names a code.
 *
 *     YTR_ERR_ARG     null handle, condition or factor or trial out of
 *                       range, an outcome below YTR_INVALID, a requeue of
 *                       a practice, warmup or track trial, a restore with
 *                       more outcomes than the session has trials, a save
 *                       buffer smaller than ytr_save_size()
 *     YTR_ERR_CLOSED  the handle is not open
 *     YTR_ERR_ORDER   update() or requeue() with no pending trial (next()
 *                       twice is fine), restore() on a handle that has
 *                       already handed out a trial
 *     YTR_ERR_FULL    next() with YTR_MAX_TRIALS trials in the history
 *                       and something left to run; a re-queue that will not
 *                       fit (refused, the outcome recorded as YTR_INVALID)
 *     YTR_ERR_IO      (v0.3) a data file write failed, now or before
 *
 *   The data writer: ytr_data_open() and ytr_data_close() return bool and
 *   fill ytr_data_error(); ytr_data_write() fills it on YTR_ERR_IO. The
 *   setters and write return 0 or a code: YTR_ERR_ARG for a null handle,
 *   a column out of range, not a SET column or of another type, or a NULL
 *   row with a bound column; YTR_ERR_CLOSED; YTR_ERR_ORDER as DATA FILE
 *   says. ytr_data_open() rejects, with a message naming the column: a
 *   null desc; no path and no write function; no columns; more than
 *   YTR_DATA_MAX_COLS; a name that breaks the rule or repeats one; a
 *   type out of range; decimals out of range or on a type without them; a
 *   field whose size is not the type's, or a CHARS field without a size;
 *   CONST without text or with too much; a flush out of range; a session
 *   that is not open, or whose line could pass YTR_DATA_BUF; a meta_path
 *   over 1023 bytes; a file or sidecar that cannot be created or written.
 *
 *   On a closed or null handle the counts (n_conditions, n_factors,
 *   n_scheduled, n_run, n_done, n_valid) are 0, the lookups (level,
 *   condition_at, condition_from_levels, count) and the loop functions
 *   return the error, proportion is NaN, history and record are NULL,
 *   done and is_open are false, and save_size is 0.
 *
 *   ---------------------------------------------------------------------
 *   MEMORY AND THREADS
 *   ---------------------------------------------------------------------
 *   The handle holds the condition table, the schedule and the history
 *   inline: 19 bytes per trial at YTR_MAX_TRIALS (4096) plus 12 per
 *   condition at YTR_MAX_CONDITIONS (1024) plus the desc, 91480
 *   bytes on a 64-bit ABI in v0.1; v0.2 adds scratch for open() and the
 *   unit moves (2 x 2 bytes per trial), the group order (2 bytes per
 *   condition) and the table's level pointers: 110528 bytes; v0.2.1 adds
 *   8 bytes per trial per YTR_MAX_JITTERS (4) for the draws and 1.7 KB
 *   for the jitters' settings: 243344 bytes (YTR_MAX_JITTERS 1: 143744).
 *   Nothing allocates. The caller's per-trial
 *   records live in desc.records, a buffer of at least record_size x
 *   YTR_MAX_TRIALS bytes the caller provides. Define YTR_MAX_TRIALS or
 *   YTR_MAX_CONDITIONS before the include to resize; both must stay
 *   below 32768, since trial and condition numbers are stored in 16 bits.
 *   A handle is not thread-safe; one thread runs one session.
 *   The data writer's handle (v0.3) holds the column list, the CONST
 *   texts and the row buffer: 18,856 bytes on a 64-bit ABI at the
 *   defaults (YTR_DATA_BUF 8192). ytr_data_write() allocates nothing and
 *   makes one write call per row; the C library allocates the FILE at
 *   open, and the file has no stdio buffer to allocate. One thread writes
 *   one file.
 *   Costs: open() is O(trials) plus, per repair swap, up to 128 slot
 *   checks for the partner and a rescan from the lower slot, each check
 *   O(constraints x rule span), so an impossible
 *   design spends the whole swap budget (tests/adapt/trials_test.c
 *   prints what that costs: 60 ms for a 4000-trial impossible design at
 *   the default budget, gcc -O2 on a desktop x64). v0.2: a table, an order
 *   list, a subset and draws are O(trials) at open (draws O(trials x log
 *   rows)); units shuffle in O(units) and repair by moves of O(slots)
 *   each; balance is O(levels^2 x lambda) for the circuit plus a shuffle;
 *   groups repair inside each group. STATUS has measured times. next()
 *   is O(tracks + constraints x rule span), plus O(slots left x
 *   constraints x rule span) when a scheduled trial has to be moved
 *   forward. update() is O(1) plus the record copy;
 *   requeue() is O(slots left); count() for an outcome other than 1 is
 *   O(trials). "Rule span" is n for max_run, w for max_in_window, g for
 *   min_gap and 1 for the rest.
 *
 *   ---------------------------------------------------------------------
 *   BUILDING
 *   ---------------------------------------------------------------------
 *   Nothing to link; ysp/table.h must sit beside this header. Define
 *   YTR_API (and YTB_API for ysp/table.h) to override the default `extern`
 *   linkage (definitions too, so static works; a -DYTR_API=static build
 *   needs a translation unit that calls every function, or
 *   -Wno-unused-function).
 *
 *       cc -O2 -Iinclude -o trials_mocs examples/trials/mocs.c
 *       cl /O2 /Iinclude examples\trials\mocs.c
 *
 *   ---------------------------------------------------------------------
 *   LICENSE: public domain / MIT-0, see end of file.
 */
#ifndef YSP_TRIALS_H_INCLUDED
#define YSP_TRIALS_H_INCLUDED

/* The version of this header, for a log line or a compile-time check. The
 * string is the three numbers, and the test asserts that it stays so. */
#define YTR_VERSION_MAJOR 0
#define YTR_VERSION_MINOR 3
#define YTR_VERSION_PATCH 0
#define YTR_VERSION_STRING "0.3.0"

/* fsync() and fileno() for YTR_FLUSH_SYNC under -std=c11: the feature-test
 * macro of ysp/rt.h and ysp/pack.h, set before the first system header
 * (Linux and Emscripten only). */
#if (defined(__linux__) || defined(__EMSCRIPTEN__)) && !defined(_DEFAULT_SOURCE) && \
    !defined(_GNU_SOURCE) && !defined(_POSIX_C_SOURCE) && !defined(_XOPEN_SOURCE)
    #define _DEFAULT_SOURCE 1
#endif

/* Tables (conditions files, trial lists) are ysp/table.h's. */
#include "ysp/table.h"

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifndef YTR_API
#define YTR_API extern
#endif

#ifndef YTR_MAX_TRIALS
#define YTR_MAX_TRIALS 4096
#endif
#ifndef YTR_MAX_CONDITIONS
#define YTR_MAX_CONDITIONS 1024
#endif
#define YTR_MAX_FACTORS      8
#define YTR_MAX_CONSTRAINTS 16
#define YTR_MAX_TRACKS      16
/* Jitters per session (v0.2.1): each costs 8 bytes per trial in the
 * handle, so a session that needs fewer may define it lower. */
#ifndef YTR_MAX_JITTERS
#define YTR_MAX_JITTERS      4
#endif
#define YTR_MAX_JITTER_VALUES 32   /* values of one CHOICE jitter        */
#define YTR_MAX_JITTER_NAME   31   /* bytes of a jitter's name           */
#define YTR_JITTER_MAX_S   1.0e6   /* the longest duration, seconds      */

/* The history and the schedule store trial and condition numbers in 16
 * bits to keep the handle small. */
#if YTR_MAX_TRIALS < 1 || YTR_MAX_TRIALS > 32767
#error "YTR_MAX_TRIALS must be in [1, 32767]"
#endif
#if YTR_MAX_JITTERS < 1 || YTR_MAX_JITTERS > 64
#error "YTR_MAX_JITTERS must be in [1, 64]"
#endif
#if YTR_MAX_CONDITIONS < 1 || YTR_MAX_CONDITIONS > 32767
#error "YTR_MAX_CONDITIONS must be in [1, 32767]"
#endif

/* --- codes ------------------------------------------------------------- */

#define YTR_DONE        (-1)  /* ytr_next(): nothing left to run        */
#define YTR_ERR_ARG     (-2)
#define YTR_ERR_CLOSED  (-3)
#define YTR_ERR_ORDER   (-4)
#define YTR_ERR_FULL    (-5)
#define YTR_ERR_IO      (-6)  /* the data file could not be written (v0.3) */

#define YTR_INVALID     (-1)  /* an outcome that is no response           */
#define YTR_REQUEUE     (-2)  /* the outcome ytr_requeue() records, and
                                 * what ytr_restore() reads as a re-queue */
#define YTR_CONDITION   (-1)  /* "the condition row" as a constraint's factor */
#define YTR_ANY_LEVEL   (-1)  /* "whatever level it is" in a constraint    */

/* Static description of a YTR_ERR_* code ("ok" for values >= 0). */
YTR_API const char* ytr_strerror(int code);

/* YTR_VERSION_STRING as compiled into the implementation. */
YTR_API const char* ytr_version(void);

/* --- description ------------------------------------------------------- */

typedef enum ytr_order {
    YTR_ORDER_SEQUENTIAL = 0,
    YTR_ORDER_RANDOM,        /* shuffled within each repetition          */
    YTR_ORDER_FULL_RANDOM,   /* one shuffle over everything              */
    YTR_ORDER_CONSTRAINED,   /* FULL_RANDOM repaired to the constraints  */
    YTR_ORDER_LIST,          /* desc.order_list, as given (v0.2)         */
    YTR_ORDER_WITH_REPLACEMENT /* desc.draws independent draws (v0.2)    */
} ytr_order;

typedef enum ytr_interleave {
    YTR_INTERLEAVE_RANDOM = 0, /* by weight                              */
    YTR_INTERLEAVE_ROUND_ROBIN
} ytr_interleave;

typedef enum ytr_rule {
    YTR_RULE_MAX_RUN = 0,
    YTR_RULE_MAX_IN_WINDOW,
    YTR_RULE_MIN_GAP,
    YTR_RULE_NO_TRANSITION,
    YTR_RULE_FIRST_NOT,
    /* v0.2. Structural: they shape the order and are never repaired. */
    YTR_RULE_FOLLOWED_BY,    /* every `level` directly followed by a `level2` */
    YTR_RULE_PRECEDED_BY,    /* every `level` directly preceded by a `level2` */
    YTR_RULE_CHUNK,          /* contiguous rows with one value: one unit      */
    YTR_RULE_BALANCE         /* every ordered level pair adjacent equally often */
} ytr_rule;

/* ytr_balance_flags() options; zero is ytr_balance(). */
#define YTR_BALANCE_NO_REPEAT 1  /* no level twice in a row (no self pairs) */
#define YTR_BALANCE_NO_LEADIN 2  /* no lead-in trial; one transition is missing */

/* A uniform variate in [0, 1) from the caller's generator. */
typedef double (*ytr_rng_fn)(void* ctx);

/* One splitmix64 step on the uint64_t at `state`, returned as a double in
 * [0, 1) built from the top 53 bits. The state is the caller's; pass its
 * address as rng_ctx. */
YTR_API double ytr_splitmix(void* state);

typedef struct ytr_factor_desc {
    const char* name;    /* for ytr_format_header(); may be NULL         */
    int         n_levels;
} ytr_factor_desc;

/* One ordering constraint; build with the helpers below. */
typedef struct ytr_constraint {
    ytr_rule rule;
    int factor;   /* factor index, or YTR_CONDITION                      */
    int level;    /* level index, or YTR_ANY_LEVEL (MAX_RUN, MAX_IN_WINDOW,
                   * MIN_GAP); NO_TRANSITION: the level that comes first    */
    int level2;   /* NO_TRANSITION: the level that may not follow;
                   * FOLLOWED_BY, PRECEDED_BY: the partner level           */
    int n;        /* MAX_RUN: run length; MAX_IN_WINDOW: count; MIN_GAP: gap;
                   * BALANCE: YTR_BALANCE_* flags                        */
    int window;   /* MAX_IN_WINDOW: window length                          */
} ytr_constraint;

YTR_API ytr_factor_desc ytr_factor(const char* name, int n_levels);
YTR_API ytr_constraint  ytr_max_run(int factor, int level, int max_run);
YTR_API ytr_constraint  ytr_max_in_window(int factor, int level, int window, int max_count);
YTR_API ytr_constraint  ytr_min_gap(int factor, int level, int gap);
YTR_API ytr_constraint  ytr_no_transition(int factor, int from_level, int to_level);
YTR_API ytr_constraint  ytr_first_not(int factor, int level);
/* v0.2 structural rules; see STRUCTURE. */
YTR_API ytr_constraint  ytr_followed_by(int factor, int level, int next_level);
YTR_API ytr_constraint  ytr_preceded_by(int factor, int level, int prev_level);
YTR_API ytr_constraint  ytr_chunk(int factor);
YTR_API ytr_constraint  ytr_balance(int factor);
YTR_API ytr_constraint  ytr_balance_no_repeat(int factor);
YTR_API ytr_constraint  ytr_balance_flags(int factor, int flags);

/* Groups (v0.2): the levels of one factor as an outer order; see GROUPS. */
typedef enum ytr_group_mode {
    YTR_GROUPS_NONE = 0,
    YTR_GROUPS_BLOCKED,      /* each group's trials together; a group is a block */
    YTR_GROUPS_ALTERNATE     /* one trial from each group in turn              */
} ytr_group_mode;

typedef enum ytr_group_order {
    YTR_GROUP_ORDER_SEQUENTIAL = 0,  /* level order                          */
    YTR_GROUP_ORDER_RANDOM,          /* one shuffle at open                  */
    YTR_GROUP_ORDER_LATIN,           /* row `participant` of a cyclic square */
    YTR_GROUP_ORDER_BALANCED_LATIN,  /* row `participant` of a Williams square */
    YTR_GROUP_ORDER_LIST             /* `list`: level numbers in run order   */
} ytr_group_order;

typedef struct ytr_group_desc {
    ytr_group_mode  mode;        /* NONE: no groups, the rest is ignored     */
    int               factor;      /* the factor whose levels are the groups  */
    ytr_group_order order;
    int               participant; /* LATIN, BALANCED_LATIN: the square's row  */
    const int*        list;        /* LIST: group levels in run order          */
    int               n_list;
} ytr_group_desc;

/* Row `row` of an n x n Latin square into out[n]: cyclic, out[j] = (row + j)
 * mod n, or balanced (Williams 1949). Returns the number of rows of the
 * design (n; 2n for a balanced square of odd n), `row` being taken modulo
 * it, or YTR_ERR_ARG for n < 1, n > YTR_MAX_CONDITIONS, a negative row
 * or a NULL out. See LATIN SQUARES. */
YTR_API int ytr_latin(int n, int row, bool balanced, int* out);

/* --- jitter (v0.2.1) --------------------------------------------------- */

typedef enum ytr_jitter_dist {
    YTR_JITTER_UNIFORM = 0,      /* [lo, hi]                                */
    YTR_JITTER_CHOICE,           /* one of values[], each equally likely    */
    YTR_JITTER_EXPONENTIAL       /* lo + an exponential of mean `scale`,
                                    * truncated at hi                         */
} ytr_jitter_dist;

/* A random duration, in seconds (JITTER). Zero-initialize, or use the
 * helpers below. */
typedef struct ytr_jitter_desc {
    const char*       name;        /* data-line column; [A-Za-z_][A-Za-z0-9_]* */
    ytr_jitter_dist dist;
    double            lo, hi;      /* UNIFORM, EXPONENTIAL: the interval      */
    double            scale;       /* EXPONENTIAL: 1 / hazard, seconds        */
    const double*     values;      /* CHOICE                                  */
    int               n_values;
    int               rate_num;    /* > 0: whole frames of rate_num / rate_den */
    int               rate_den;    /* Hz (0 = 1); 0 / 0 = continuous          */
    const char*       lo_column;   /* desc level: per-condition lo, hi, scale */
    const char*       hi_column;   /* from these table columns; NULL = the    */
    const char*       scale_column;/* field above                             */
} ytr_jitter_desc;

/* One drawn duration. frames is -1 unless the jitter snaps to frames; an
 * invalid desc gives s = NaN, ns = -1, frames = -1. */
typedef struct ytr_jitter_value {
    double  s;
    int64_t ns;
    int64_t frames;
} ytr_jitter_value;

YTR_API ytr_jitter_desc ytr_uniform(const char* name, double lo, double hi);
YTR_API ytr_jitter_desc ytr_choice(const char* name, const double* values, int n);
YTR_API ytr_jitter_desc ytr_exponential(const char* name, double lo, double hi, double scale);
/* j with its draws snapped to whole frames of rate_num / rate_den Hz. */
YTR_API ytr_jitter_desc ytr_frames(ytr_jitter_desc j, int rate_num, int rate_den);

/* Call level, no session: is j valid (the message says why not)? */
YTR_API bool ytr_jitter_check(const ytr_jitter_desc* j, char* err, size_t cap);
/* The duration for variate u in [0, 1) (clamped into it). */
YTR_API ytr_jitter_value ytr_jitter_map(const ytr_jitter_desc* j, double u);
/* One call of rng(ctx), then ytr_jitter_map(). */
YTR_API ytr_jitter_value ytr_jitter_draw(const ytr_jitter_desc* j, ytr_rng_fn rng, void* ctx);

/* Is the track finished? Called on the caller's handle; TRACKS says when. */
typedef bool (*ytr_done_fn)(void* ctx);

typedef struct ytr_track_desc {
    void*         ctx;
    ytr_done_fn is_done;   /* required                                   */
    double        weight;    /* RANDOM interleave; 0 = 1                   */
} ytr_track_desc;

YTR_API ytr_track_desc ytr_track(void* ctx, ytr_done_fn is_done);

/* Session description. Zero-initialize it and set only what you need.
 * Required: n_conditions >= 1 or n_factors >= 1 or n_tracks >= 1; reps >= 1
 * (or cond_reps) when there are conditions; track_rate when there are both
 * conditions and tracks; rng whenever RANDOMNESS says a draw is needed.
 * ytr_open() rejects, with a message: a null desc; a negative count;
 * more of anything than its YTR_MAX_*; a factor with no levels; a factor
 * product above YTR_MAX_CONDITIONS or unequal to a nonzero
 * n_conditions; no conditions and no tracks; conditions with reps < 1 and
 * no cond_reps, a negative cond_reps entry, or a zero sum; an order,
 * interleave or rule out of range; constraints under any order but
 * CONSTRAINED; a constraint whose factor or level is out of range, that
 * uses YTR_ANY_LEVEL where its rule takes none, or whose n, window or
 * gap is below 1; a track without is_done or with a weight that is
 * negative or not finite; a track_rate outside [0, 1], or 0 with both
 * conditions and tracks; practice or warmup with no conditions; n_warmup
 * without block_size; a warmup list entry out of range, a list longer than
 * YTR_MAX_CONDITIONS, or a count without a list; record_size without records; a missing rng; practice plus
 * scheduled (plus, without tracks, warmup) trials above YTR_MAX_TRIALS;
 * and, without tracks, constraints the repair cannot meet. */
typedef struct ytr_desc {
    int               n_conditions;  /* plain rows; 0 with factors         */
    ytr_factor_desc factors[YTR_MAX_FACTORS];
    int               n_factors;
    int               reps;          /* repetitions of every condition     */
    const int*        cond_reps;     /* per-condition repetitions, or NULL */
    ytr_order       order;
    ytr_constraint  constraints[YTR_MAX_CONSTRAINTS];
    int               n_constraints;
    int               max_swaps;     /* repair budget; 0 = 100000          */
    ytr_track_desc  tracks[YTR_MAX_TRACKS];
    int               n_tracks;
    ytr_interleave  interleave;
    double            track_rate;    /* P(track trial) while any runs      */
    int               block_size;    /* main trials per block; 0 = one     */
    bool              constraints_span_blocks; /* count runs across blocks */
    int               n_practice;    /* before the schedule                */
    int               n_warmup;      /* at the start of each later block   */
    const int*        warmup_conditions; /* rows to draw those from, or   */
    int               n_warmup_conditions; /* NULL / 0 = any condition     */
    int               requeue_gap;   /* 0 = append at the end              */
    ytr_rng_fn      rng;
    void*             rng_ctx;
    void*             records;       /* record_size x YTR_MAX_TRIALS     */
    size_t            record_size;   /* 0 = no records                     */
    /* v0.2. Zero is "not used" for each. */
    const ytb_table* table;        /* rows = conditions, columns = factors */
    const int*        order_list;    /* ORDER_LIST: rows in run order       */
    int               n_order_list;
    int               draws;         /* WITH_REPLACEMENT: trials (per group
                                      * when BLOCKED)                       */
    const double*     weights;       /* WITH_REPLACEMENT: per row; NULL = equal */
    int               subset;        /* k rows without replacement; 0 = all */
    ytr_group_desc  groups;
    /* v0.2.1: a duration drawn per trial for each (JITTER). */
    ytr_jitter_desc jitters[YTR_MAX_JITTERS];
    int               n_jitters;
} ytr_desc;

/* --- handle ------------------------------------------------------------ */

/* What ytr_next() reports about the trial it chose. */
typedef struct ytr_trial_info {
    int  index;          /* trial number, from 0                          */
    int  condition;      /* row, or -1 for a track trial                   */
    int  track;          /* track, or -1 for a condition trial             */
    int  rep;            /* repetition of a scheduled trial, else -1       */
    int  block;          /* -1 for practice                                */
    bool first_in_block;
    bool after_break;    /* a scheduled or marked break precedes it        */
    bool practice;
    bool warmup;
    bool requeued;       /* a re-run of a re-queued trial                  */
} ytr_trial_info;

/* One recorded trial. ytr_history() hands out the array. */
typedef struct ytr_trial {
    int16_t condition;   /* -1 for a track trial                           */
    int8_t  track;       /* -1 for a condition trial                       */
    uint8_t flags;       /* YTR_FLAG_*                                   */
    int16_t rep;         /* -1 for practice, warmup and track trials       */
    int16_t block;       /* -1 for practice                                */
    int32_t outcome;     /* YTR_INVALID until update(), or as recorded   */
} ytr_trial;

#define YTR_FLAG_PRACTICE        1
#define YTR_FLAG_REQUEUED        2  /* a re-run of a re-queued trial      */
#define YTR_FLAG_DONE            4  /* update() or requeue() has run      */
#define YTR_FLAG_WARMUP          8  /* a reintroduction trial after a break */
#define YTR_FLAG_AFTER_BREAK    16  /* a break preceded it (scheduled, or
                                       * ytr_mark_break)                  */
#define YTR_FLAG_FIRST_IN_BLOCK 32
#define YTR_FLAG_VIOLATION      64  /* ran against a constraint because
                                       * nothing else could; see CONSTRAINTS */
#define YTR_FLAG_LEADIN        128  /* the lead-in of a balanced order (v0.2) */

/* A jitter as the handle keeps it: name and values copied, columns
 * resolved to indices (-1 for none). Internal. */
typedef struct ytr__jit {
    char    name[YTR_MAX_JITTER_NAME + 1];
    int     dist, n_values, rate_num, rate_den;
    int     col[3];                       /* lo, hi, scale column           */
    double  lo, hi, scale;
    double  values[YTR_MAX_JITTER_VALUES];
} ytr__jit;

/* Handle. The caller allocates it and treats every field as opaque.
 * ytr_open() and ytr_load() reset it, so it may be reused. Sized by
 * YTR_MAX_TRIALS and YTR_MAX_CONDITIONS; see MEMORY AND THREADS. */
typedef struct ytr_trials {
    ytr_desc  desc;                         /* defaults resolved          */
    int         n_cond;                       /* rows                       */
    int         level_stride[YTR_MAX_FACTORS];
    int         n_scheduled;                  /* slots: practice, then main */
    int         n_run;                        /* trials handed out          */
    int         n_done;                       /* trials updated or re-queued */
    int         current;                      /* trial index of the pending
                                               * next(), or -1             */
    int         schedule_pos;                 /* next slot to hand out      */
    int         rr_next;                      /* round-robin cursor         */
    uint32_t    track_done;                   /* bit i: track i said done   */
    int         break_pending;                /* ytr_mark_break() seen   */
    int         n_main;                       /* main trials handed out     */
    int         seg_start;                    /* main index starting the
                                               * constraint segment         */
    int         warm_block;                   /* block warm_done counts for */
    int         warm_done;
    int         swaps;                        /* repair swaps used at open  */
    bool        open;
    bool        has_cond_reps;                /* desc.cond_reps was given   */
    int16_t     cond_reps[YTR_MAX_CONDITIONS]; /* copied once at open     */
    int16_t     warm_list[YTR_MAX_CONDITIONS]; /* desc.warmup_conditions */
    int16_t     schedule[YTR_MAX_TRIALS];   /* condition per slot         */
    int16_t     schedule_rep[YTR_MAX_TRIALS];
    uint8_t     schedule_flags[YTR_MAX_TRIALS];
    int16_t     main_seq[YTR_MAX_TRIALS];   /* condition per main trial,
                                               * -1 for a track trial       */
    ytr_trial history[YTR_MAX_TRIALS];
    int32_t     tally_valid[YTR_MAX_CONDITIONS];
    int32_t     tally_pos[YTR_MAX_CONDITIONS]; /* outcome == 1, the common
                                                  * case, kept O(1)         */
    char        error[256];
    /* v0.2 */
    bool        v2;                           /* a v0.2 schedule feature is
                                               * used: snapshot format 2    */
    bool        units;                        /* structural rules made units */
    bool        has_leadin;
    int         n_fac;                        /* factors: table columns or
                                               * desc.n_factors             */
    int         blk;                          /* block of the last main trial */
    int         blk_next;                     /* main index of the next grid
                                               * boundary (block_size)      */
    int         balance_ci;                   /* the BALANCE constraint, or -1 */
    uint64_t    aux_hash;                     /* weights, order list, group list */
    bool        had_weights;                  /* desc.weights was given      */
    const unsigned char* col_lev[YTB_MAX_COLUMNS]; /* table level bytes   */
    int16_t     aux[YTR_MAX_CONDITIONS];    /* group order at open        */
    int16_t     work[2][YTR_MAX_TRIALS];    /* scratch, never state       */
    /* v0.2.1: jitters, copied at open (names, values, columns resolved) */
    int         n_jit;
    ytr__jit  jit[YTR_MAX_JITTERS];
    int64_t     jit_raw[YTR_MAX_TRIALS][YTR_MAX_JITTERS]; /* per trial:
                                               * nanoseconds, or the frame
                                               * count when the jitter snaps */
} ytr_trials;

/* --- lifecycle --------------------------------------------------------- */

/* Validate `desc`, build the condition table, draw the practice trials,
 * order the schedule and repair it to the constraints. Returns false with
 * ytr_error() set on any failure, including (without tracks) an
 * unsatisfiable constraint set. It and ytr_load() are the only functions
 * that write the message buffer. */
YTR_API bool ytr_open(ytr_trials* t, const ytr_desc* desc);

/* The last open() or load() message ("" after a success). */
YTR_API const char* ytr_error(const ytr_trials* t);
YTR_API bool        ytr_is_open(const ytr_trials* t);

/* --- conditions -------------------------------------------------------- */

YTR_API int ytr_n_conditions(const ytr_trials* t);
YTR_API int ytr_n_factors(const ytr_trials* t);

/* Level index of `factor` in condition row `condition`, or YTR_ERR_ARG.
 * YTR_CONDITION as the factor returns the row itself. */
YTR_API int ytr_level(const ytr_trials* t, int condition, int factor);

/* Condition row from per-factor level indices `levels[n_factors]`, or
 * YTR_ERR_ARG. */
YTR_API int ytr_condition_from_levels(const ytr_trials* t, const int* levels);

/* Condition in slot `i` of the schedule, or YTR_ERR_ARG. Slots
 * 0..n_practice-1 are the practice trials, then the main schedule with any
 * re-queued copies. Not the trial index: track and warmup trials are not
 * scheduled. For preloading; ORDER says when an answer can change. */
YTR_API int ytr_condition_at(const ytr_trials* t, int i);
YTR_API int ytr_n_scheduled(const ytr_trials* t);

/* --- the loop ---------------------------------------------------------- */

/* Choose the next trial and describe it in `info` (may be NULL). Returns
 * its index, YTR_DONE, or a negative error. Idempotent until
 * ytr_update() or ytr_requeue(). */
YTR_API int ytr_next(ytr_trials* t, ytr_trial_info* info);

/* Record `outcome` (>= 0, or YTR_INVALID) for the pending trial and copy
 * record_size bytes from `rec` (NULL zero-fills the slot). Returns 0 or
 * YTR_ERR_*. */
YTR_API int ytr_update(ytr_trials* t, int outcome, const void* rec);

/* Record that a break was taken: before the pending trial when there is
 * one, else before the next trial handed out. Flags that trial and starts a
 * constraint segment; see BLOCKS, BREAKS, ... Returns 0 or YTR_ERR_*. */
YTR_API int ytr_mark_break(ytr_trials* t);

/* Record YTR_REQUEUE for the pending trial and schedule its condition
 * again; see BLOCKS, BREAKS, PRACTICE, WARMUP, CATCH TRIALS, RE-QUEUES.
 * Returns 0 or YTR_ERR_*. Call it INSTEAD of ytr_update() for that
 * trial. A track trial cannot be re-queued (the track decides), nor a
 * practice or warmup trial: YTR_ERR_ARG, and the trial stays pending. */
YTR_API int ytr_requeue(ytr_trials* t);

/* True when no trial is pending, the schedule is exhausted and every track
 * is done. Calls is_done() on each track not yet known to be done. */
YTR_API bool ytr_done(const ytr_trials* t);

/* Trials handed out (a pending one included), and trials completed. */
YTR_API int ytr_n_run(const ytr_trials* t);
YTR_API int ytr_n_done(const ytr_trials* t);

/* --- tallies ----------------------------------------------------------- */

/* Valid (outcome >= 0), non-practice, non-warmup trials of `condition` so
 * far; the number of its completed non-practice, non-warmup trials with
 * `outcome`; and that count over n_valid (NaN when n_valid is 0 or the
 * outcome is negative). ytr_count() for an outcome other than 1 scans
 * the history: O(trials). */
YTR_API int    ytr_n_valid(const ytr_trials* t, int condition);
YTR_API int    ytr_count(const ytr_trials* t, int condition, int outcome);
YTR_API double ytr_proportion(const ytr_trials* t, int condition, int outcome);

/* --- history ----------------------------------------------------------- */

/* The trial array and its length (a pending trial included). The pointer
 * is into the handle. NULL with *n = 0 on a closed handle. */
YTR_API const ytr_trial* ytr_history(const ytr_trials* t, int* n);

/* The caller's record for trial `i` (i < ytr_n_run()), or NULL. */
YTR_API const void* ytr_record(const ytr_trials* t, int i);

/* One newline-terminated CSV line for trial `i`: index,block,rep,
 * condition,track,practice,warmup,requeued,after_break,outcome, then one
 * column per factor level. Returns the length written or needed (snprintf
 * semantics: the line is complete when the return is below cap), or a
 * negative YTR_ERR_*. ytr_format_header() writes the matching header
 * using the factor names. */
YTR_API int ytr_format_row(const ytr_trials* t, int i, char* buf, size_t cap);
YTR_API int ytr_format_header(const ytr_trials* t, char* buf, size_t cap);

/* One newline-terminated line of space-separated key=value pairs: the
 * version, the desc's numbers, the constraints in helper-call form, the
 * repair's swap count, and trials scheduled, run and done. For the head of
 * a data file next to the seed the caller logs. snprintf semantics. */
YTR_API int ytr_format_meta(const ytr_trials* t, char* buf, size_t cap);

/* The session's order settings as rules text (GRAMMAR), one statement per
 * line, so a logged session pastes back into ytr_rules(). Names need a
 * table or factor names; a factor with no name is written f<index>.
 * snprintf semantics. */
YTR_API int ytr_format_rules(const ytr_trials* t, char* buf, size_t cap);

/* --- rules text (v0.2) ------------------------------------------------- */

typedef struct ytr_rules_desc {
    const char*        text;        /* the rules, UTF-8                      */
    size_t             len;
    const ytb_table* table;       /* names resolve against its columns;
                                     * NULL: against desc.factors[].name     */
    int                participant; /* for @participant                      */
    void*              arena;       /* holds the arrays the rules produce   */
    size_t             arena_size;
} ytr_rules_desc;

/* Apply rules text to `d` (which may already hold rng, records, tracks).
 * Returns 0, or the 1-based line of the first error with the message in
 * err. The arrays it makes live in the arena; open() and load() copy or
 * read them, so the arena may go away after open(). See GRAMMAR. */
YTR_API int ytr_rules(ytr_desc* d, const ytr_rules_desc* r, char* err, size_t cap);

/* Snapshot: bytes needed (0 on a closed handle); write them (returns the
 * bytes written, or YTR_ERR_ARG when cap is too small); and rebuild from
 * them. `desc` re-supplies the pointer fields and must agree with the
 * snapshot on every number; a mismatch, a wrong magic or format, or a
 * truncated or corrupt snapshot fails the load with a message in
 * ytr_error(). SAVING, REPLAY AND RESUME gives the layout. */
YTR_API size_t ytr_save_size(const ytr_trials* t);
YTR_API int    ytr_save(const ytr_trials* t, void* buf, size_t cap);
YTR_API bool   ytr_load(ytr_trials* t, const ytr_desc* desc, const void* buf, size_t len);

/* Re-apply `n` outcomes (and records, record_size bytes each, or NULL) to a
 * freshly opened handle with the same desc and seed, advancing the schedule
 * as the original run did. An outcome of YTR_REQUEUE re-queues. Returns 0
 * or YTR_ERR_*; SAVING, REPLAY AND RESUME gives the limits. */
YTR_API int ytr_restore(ytr_trials* t, const int* outcomes, const void* records, int n);

/* --- jitter values (v0.2.1) ------------------------------------------- */

/* Jitter j of trial i (ytr_trial_info.index): seconds, nanoseconds and,
 * for a jitter that snaps, whole frames. Invalid i or j: s NaN, ns -1. */
YTR_API ytr_jitter_value ytr_jitter(const ytr_trials* t, int i, int j);
/* The index of the jitter named `name`, or -1. */
YTR_API int ytr_jitter_index(const ytr_trials* t, const char* name);

/* --- data file (v0.3.0) ------------------------------------------------ */

/* A column's type: the C type it reads and how it is written (DATA FILE). */
typedef enum ytr_col_type {
    YTR_COL_INT = 1,   /* int; YTR_MISSING_INT is missing                 */
    YTR_COL_INT64,     /* int64_t; YTR_MISSING_I64 is missing             */
    YTR_COL_DOUBLE,    /* double; NaN and infinities are missing          */
    YTR_COL_BOOL,      /* bool, written 1 or 0                            */
    YTR_COL_STRING,    /* const char*, UTF-8; NULL is missing             */
    YTR_COL_CHARS,     /* char[size], UTF-8, to the first NUL or the end  */
    YTR_COL_TIME_NS,   /* int64_t nanoseconds, written as seconds;
                        * YTR_MISSING_NS is missing                       */
    YTR_COL_CONST      /* the same text on every row (participant, session) */
} ytr_col_type;

#define YTR_MISSING_INT INT_MIN
#define YTR_MISSING_I64 INT64_MIN
#define YTR_MISSING_NS  INT64_MIN
#define YTR_UNBOUND     ((size_t)-1)  /* a column set per row by index      */

/* One column of the caller's. Build it with the macros below. */
typedef struct ytr_col {
    const char*    name;      /* [A-Za-z_][A-Za-z0-9_]*, at most 63 bytes   */
    ytr_col_type type;
    size_t         offset;    /* in the row struct, or YTR_UNBOUND          */
    size_t         size;      /* the field's bytes; 0 = the type's size     */
    int            decimals;  /* DOUBLE: 0 = shortest exact, 1..17 fixed;
                               * TIME_NS: 0 = 9 (exact), 1..9               */
    const char*    text;      /* CONST: the value, copied at open           */
} ytr_col;

/* A field of struct T, named as the field. Constant expressions, so a
 * column list can be a static array in C and C++. */
#define YTR_COL(T, field, type) \
    { #field, (type), offsetof(T, field), sizeof(((T*)0)->field), 0, NULL }
/* The same with `decimals` (DOUBLE: fixed decimals; TIME_NS: 1..9). */
#define YTR_COL_D(T, field, type, decimals) \
    { #field, (type), offsetof(T, field), sizeof(((T*)0)->field), (decimals), NULL }
/* A column set per row with ytr_data_set_*(); empty in a row that did not. */
#define YTR_COL_SET(name, type) { (name), (type), YTR_UNBOUND, 0, 0, NULL }
/* A column with the same text in every row. */
#define YTR_COL_TEXT(name, text) { (name), YTR_COL_CONST, YTR_UNBOUND, 0, 0, (text) }

/* When a row reaches the file (DATA FILE: FLUSH). */
typedef enum ytr_flush {
    YTR_FLUSH_ROW = 0,  /* each row handed to the OS: survives a crash or a
                         * killed process, not a power cut                 */
    YTR_FLUSH_SYNC      /* ROW, then the OS writes it to the disk          */
} ytr_flush;

/* Bytes out, for a writer with no path: return false on a failure. */
typedef bool (*ytr_write_fn)(void* ctx, const void* bytes, size_t n);

/* Zero-initialize and set what you need. Required: a path or a write
 * function, and at least one column (the session's or cols[]). */
typedef struct ytr_data_desc {
    const char*        path;      /* UTF-8; created or truncated             */
    ytr_write_fn     write;     /* used when path is NULL                  */
    void*              write_ctx;
    const ytr_trials* trials;   /* the session's columns first; NULL: none */
    const ytr_col*   cols;      /* the caller's columns, copied at open    */
    int                n_cols;
    ytr_flush        flush;
    const char*        meta_path; /* a sidecar of key=value lines, or NULL  */
    const char*        meta;      /* the caller's lines for it, or NULL     */
} ytr_data_desc;

#ifndef YTR_DATA_BUF
#define YTR_DATA_BUF 8192         /* the row buffer: rows longer are split  */
#endif
#define YTR_DATA_MAX_COLS 64      /* all columns: ysp/table.h reads 64       */
#define YTR_DATA_POOL     2048    /* bytes of the CONST columns' text        */

/* A column as the writer keeps it. Internal. */
typedef struct ytr__dcol {
    char     name[64];
    int      type, decimals;
    size_t   offset, size;
    int      text_off, text_len;  /* CONST: its CSV text in the pool         */
    bool     set;                  /* a value is set for this row            */
    union { int64_t i; double d; const char* s; } v;
} ytr__dcol;

/* The data writer. The caller allocates it (about 18 KB: static or on
 * the heap) and treats every field as opaque. */
typedef struct ytr_data {
    bool               open;
    bool               io_failed;
    ytr_flush        flush;
    void*              file;       /* FILE*, or NULL with a write function  */
    ytr_write_fn     write;
    void*              write_ctx;
    const ytr_trials* trials;
    int                next_trial; /* the first trial not yet written        */
    int                n_cols;
    int64_t            rows;
    size_t             len;        /* bytes in buf                           */
    size_t             pool_len;
    char               meta_path[1024];
    ytr__dcol        col[YTR_DATA_MAX_COLS];
    char               pool[YTR_DATA_POOL];
    char               buf[YTR_DATA_BUF];
    char               error[256];
} ytr_data;

/* Check the columns, open the file (or take the write function), write the
 * header line and, with meta_path, the sidecar. Returns false with
 * ytr_data_error() set. */
YTR_API bool ytr_data_open(ytr_data* w, const ytr_data_desc* desc);
/* The last failure's message ("" when there is none). */
YTR_API const char* ytr_data_error(const ytr_data* w);
/* The index of the caller column named `name` in desc.cols, or -1. */
YTR_API int ytr_data_col(const ytr_data* w, const char* name);

/* Set column `col` (an index into desc.cols, of a YTR_COL_SET column of
 * that type) for the next row. A string must stay valid until the row is
 * written. Return 0 or YTR_ERR_ARG / YTR_ERR_CLOSED. */
YTR_API int ytr_data_set_int(ytr_data* w, int col, int v);
YTR_API int ytr_data_set_int64(ytr_data* w, int col, int64_t v);
YTR_API int ytr_data_set_double(ytr_data* w, int col, double v);
YTR_API int ytr_data_set_bool(ytr_data* w, int col, bool v);
YTR_API int ytr_data_set_string(ytr_data* w, int col, const char* v);
YTR_API int ytr_data_set_time_ns(ytr_data* w, int col, int64_t ns);

/* Write one row: the session's columns of its last completed trial (when
 * desc.trials is set), then the caller's columns from `row` (a struct the
 * bound columns point into; NULL only when no column is bound) and from the
 * values set since the last row. With a session, call it after
 * ytr_update() or ytr_requeue() and before the next ytr_next(). Returns 0,
 * or YTR_ERR_ORDER (a trial is pending, or its row is written already),
 * YTR_ERR_ARG, YTR_ERR_CLOSED, or YTR_ERR_IO with ytr_data_error() set. */
YTR_API int ytr_data_write(ytr_data* w, const void* row);
/* Rows written. */
YTR_API int64_t ytr_data_rows(const ytr_data* w);
/* Flush and close the file, and end the sidecar. Returns false when a
 * write failed at any time, with ytr_data_error() set. */
YTR_API bool ytr_data_close(ytr_data* w);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* YSP_TRIALS_H_INCLUDED */

/* ======================================================================= *
 *                             IMPLEMENTATION                              *
 * ======================================================================= */
#ifdef YSP_TRIALS_IMPLEMENTATION
#ifndef YSP_TRIALS_IMPLEMENTATION_GUARD
#define YSP_TRIALS_IMPLEMENTATION_GUARD

#ifndef YSP_TABLE_IMPLEMENTATION_GUARD
    #define YSP_TABLE_IMPLEMENTATION
    #include "ysp/table.h"
#endif

#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <io.h>       /* _commit, for YTR_FLUSH_SYNC */
#include <share.h>    /* _SH_DENYWR */
#include <wchar.h>
#else
#include <unistd.h>   /* fsync */
#endif

#ifdef NAN
    #define YTR__NAN ((double)NAN)
#else
    /* No NAN macro means a pre-C99 library; inf - inf is the portable
     * spelling and this branch never compiles on the supported toolchains. */
    #define YTR__NAN (HUGE_VAL - HUGE_VAL)
#endif

#define YTR__DEFAULT_SWAPS 100000
#define YTR__SNAP_FORMAT   1u   /* a v0.1 desc            */
#define YTR__SNAP_FORMAT2  2u   /* a desc with v0.2 fields */
#define YTR__SNAP_FORMAT3  3u   /* a desc with jitters (v0.2.1): format 2
                                   * plus the jitters and their draws */

/* break_pending bits: the flag goes on the next trial of any kind, the
 * segment starts at the next MAIN trial, and those can differ when the next
 * trial is a warmup. */
#define YTR__BREAK_FLAG 1
#define YTR__BREAK_SEG  2

/* isfinite() is C99 and MSVC's default C dialect does not declare it, so the
 * test is written in comparisons a NaN loses on its own. */
static bool ytr__finite(double x) {
    return x > -HUGE_VAL && x < HUGE_VAL;
}

/* --- codes, version, helpers ------------------------------------------- */

YTR_API const char* ytr_strerror(int code) {
    switch (code) {
    case YTR_DONE:       return "no trial left";
    case YTR_ERR_ARG:    return "invalid argument";
    case YTR_ERR_CLOSED: return "session is not open";
    case YTR_ERR_ORDER:  return "call out of order";
    case YTR_ERR_FULL:   return "trial history is full";
    case YTR_ERR_IO:     return "data file write failed";
    default:               return code >= 0 ? "ok" : "unknown error";
    }
}

YTR_API const char* ytr_version(void) { return YTR_VERSION_STRING; }

YTR_API double ytr_splitmix(void* state) {
    uint64_t* s = (uint64_t*)state;
    uint64_t z;
    *s += 0x9E3779B97F4A7C15ULL;
    z = *s;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z = z ^ (z >> 31);
    return (double)(z >> 11) * (1.0 / 9007199254740992.0);
}

YTR_API ytr_factor_desc ytr_factor(const char* name, int n_levels) {
    ytr_factor_desc f;
    f.name = name;
    f.n_levels = n_levels;
    return f;
}

static ytr_constraint ytr__constraint(ytr_rule rule, int factor, int level,
                                          int level2, int n, int window) {
    ytr_constraint c;
    c.rule = rule;
    c.factor = factor;
    c.level = level;
    c.level2 = level2;
    c.n = n;
    c.window = window;
    return c;
}

YTR_API ytr_constraint ytr_max_run(int factor, int level, int max_run) {
    return ytr__constraint(YTR_RULE_MAX_RUN, factor, level, 0, max_run, 0);
}

YTR_API ytr_constraint ytr_max_in_window(int factor, int level, int window,
                                               int max_count) {
    return ytr__constraint(YTR_RULE_MAX_IN_WINDOW, factor, level, 0, max_count, window);
}

YTR_API ytr_constraint ytr_min_gap(int factor, int level, int gap) {
    return ytr__constraint(YTR_RULE_MIN_GAP, factor, level, 0, gap, 0);
}

YTR_API ytr_constraint ytr_no_transition(int factor, int from_level, int to_level) {
    return ytr__constraint(YTR_RULE_NO_TRANSITION, factor, from_level, to_level, 0, 0);
}

YTR_API ytr_constraint ytr_first_not(int factor, int level) {
    return ytr__constraint(YTR_RULE_FIRST_NOT, factor, level, 0, 0, 0);
}

YTR_API ytr_constraint ytr_followed_by(int factor, int level, int next_level) {
    return ytr__constraint(YTR_RULE_FOLLOWED_BY, factor, level, next_level, 0, 0);
}

YTR_API ytr_constraint ytr_preceded_by(int factor, int level, int prev_level) {
    return ytr__constraint(YTR_RULE_PRECEDED_BY, factor, level, prev_level, 0, 0);
}

YTR_API ytr_constraint ytr_chunk(int factor) {
    return ytr__constraint(YTR_RULE_CHUNK, factor, 0, 0, 0, 0);
}

YTR_API ytr_constraint ytr_balance(int factor) {
    return ytr__constraint(YTR_RULE_BALANCE, factor, 0, 0, 0, 0);
}

YTR_API ytr_constraint ytr_balance_no_repeat(int factor) {
    return ytr__constraint(YTR_RULE_BALANCE, factor, 0, 0, YTR_BALANCE_NO_REPEAT, 0);
}

YTR_API ytr_constraint ytr_balance_flags(int factor, int flags) {
    return ytr__constraint(YTR_RULE_BALANCE, factor, 0, 0, flags, 0);
}

YTR_API ytr_track_desc ytr_track(void* ctx, ytr_done_fn is_done) {
    ytr_track_desc d;
    d.ctx = ctx;
    d.is_done = is_done;
    d.weight = 0.0;
    return d;
}

/* --- jitter ------------------------------------------------------------- */

YTR_API ytr_jitter_desc ytr_uniform(const char* name, double lo, double hi) {
    ytr_jitter_desc j;
    memset(&j, 0, sizeof(j));
    j.name = name;
    j.dist = YTR_JITTER_UNIFORM;
    j.lo = lo;
    j.hi = hi;
    return j;
}

YTR_API ytr_jitter_desc ytr_choice(const char* name, const double* values, int n) {
    ytr_jitter_desc j;
    memset(&j, 0, sizeof(j));
    j.name = name;
    j.dist = YTR_JITTER_CHOICE;
    j.values = values;
    j.n_values = n;
    return j;
}

YTR_API ytr_jitter_desc ytr_exponential(const char* name, double lo, double hi, double scale) {
    ytr_jitter_desc j = ytr_uniform(name, lo, hi);
    j.dist = YTR_JITTER_EXPONENTIAL;
    j.scale = scale;
    return j;
}

YTR_API ytr_jitter_desc ytr_frames(ytr_jitter_desc j, int rate_num, int rate_den) {
    j.rate_num = rate_num;
    j.rate_den = rate_den;
    return j;
}

/* The numbers one draw needs: a jitter's own, or with a row's columns. */
typedef struct ytr__jp {
    int           dist, n_values, num, den;     /* num 0: continuous       */
    double        lo, hi, scale;
    const double* values;
} ytr__jp;

/* --- exact integer arithmetic for the draws ------------------------------
 * Every draw is computed in integers from the variate's 53 bits, so it is
 * the same on every C library, compiler and flag set (-Ofast, FMA
 * contraction): no libm call and no floating-point operation whose result
 * could round differently. 128-bit values are two words. */
typedef struct ytr__u128 { uint64_t hi, lo; } ytr__u128;

/* The compiler's 128-bit integers where it has them; the same exact
 * results from 32-bit pieces elsewhere (YTR__NO_U128 forces those, for
 * the test). */
#if defined(__SIZEOF_INT128__) && !defined(YTR__NO_U128)
__extension__ typedef unsigned __int128 ytr__u128n;
#define YTR__HAVE_U128N 1
#elif defined(_MSC_VER) && defined(_M_X64) && _MSC_VER >= 1920 && !defined(YTR__NO_U128)
#include <intrin.h>
#define YTR__HAVE_UDIV128 1
#endif

static ytr__u128 ytr__mul64(uint64_t a, uint64_t b) {
#if defined(YTR__HAVE_U128N)
    ytr__u128n p = (ytr__u128n)a * b;
    ytr__u128 r;
    r.hi = (uint64_t)(p >> 64);
    r.lo = (uint64_t)p;
    return r;
#elif defined(YTR__HAVE_UDIV128)
    ytr__u128 r;
    r.lo = _umul128(a, b, &r.hi);
    return r;
#else
    uint64_t a0 = a & 0xFFFFFFFFu, a1 = a >> 32, b0 = b & 0xFFFFFFFFu, b1 = b >> 32;
    uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    uint64_t mid = (p00 >> 32) + (p01 & 0xFFFFFFFFu) + (p10 & 0xFFFFFFFFu);
    ytr__u128 r;
    r.lo = (mid << 32) | (p00 & 0xFFFFFFFFu);
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
#endif
}

static ytr__u128 ytr__add64(ytr__u128 x, uint64_t v) {
    x.lo += v;
    if (x.lo < v) x.hi++;
    return x;
}

/* floor(x / 2^sh), sh in [0, 127]; the caller knows it fits 64 bits. */
static uint64_t ytr__shr(ytr__u128 x, int sh) {
    if (sh <= 0) return x.lo;
    if (sh >= 128) return 0;
    if (sh >= 64) return x.hi >> (sh - 64);
    return (x.lo >> sh) | (x.hi << (64 - sh));
}

/* x / 2^sh rounded to nearest, ties up. */
static uint64_t ytr__shr_round(ytr__u128 x, int sh) {
    if (sh <= 0) return x.lo;
    if (sh > 128) return 0;
    return ytr__shr(x, sh) + (uint64_t)(sh <= 64 ? (x.lo >> (sh - 1)) & 1u : (x.hi >> (sh - 65)) & 1u);
}

/* floor(x / d); the quotient must fit 64 bits (x.hi < d). */
static uint64_t ytr__div(ytr__u128 x, uint64_t d) {
#if defined(YTR__HAVE_U128N)
    return (uint64_t)((((ytr__u128n)x.hi << 64) | x.lo) / d);
#elif defined(YTR__HAVE_UDIV128)
    uint64_t rem;
    return _udiv128(x.hi, x.lo, d, &rem);
#else
    uint64_t r = x.hi, q = 0, top;
    int i;
    for (i = 63; i >= 0; i--) {
        top = r >> 63;
        r = (r << 1) | ((x.lo >> i) & 1u);
        q <<= 1;
        if (top || r >= d) {
            r -= d;
            q |= 1u;
        }
    }
    return q;
#endif
}

/* x 2^sh, sh in [0, 127]; the caller knows it fits. */
static ytr__u128 ytr__shl(ytr__u128 x, int sh) {
    if (sh <= 0) return x;
    if (sh >= 64) {
        x.hi = x.lo << (sh - 64);
        x.lo = 0;
    } else {
        x.hi = (x.hi << sh) | (x.lo >> (64 - sh));
        x.lo <<= sh;
    }
    return x;
}

static int ytr__bitlen(uint64_t v) {
    int n = 0;
    while (v) { n++; v >>= 1; }
    return n;
}

/* Seconds to the nearest nanosecond, ties up, from the double's bits.
 * x is finite and in [0, YTR_JITTER_MAX_S] (or -0). */
static int64_t ytr__s_to_ns(double x) {
    uint64_t b, m;
    int e;
    memcpy(&b, &x, sizeof(b));
    b &= ~((uint64_t)1 << 63);
    e = (int)(b >> 52);
    m = b & (((uint64_t)1 << 52) - 1u);
    if (e == 0) e = 1;                      /* subnormal */
    else m |= (uint64_t)1 << 52;
    if (m == 0) return 0;
    /* x = m 2^(e - 1075); x < 2^52 s, so the shift is positive. */
    return (int64_t)ytr__shr_round(ytr__mul64(m, 1000000000u), 1075 - e);
}

/* The variate's 53 bits: floor(u 2^53), u clamped to [0, 1). A product
 * by a power of two is exact, so no flag changes it. */
static uint64_t ytr__u53(double u) {
    const double two53 = 9007199254740992.0;
    if (!(u > 0.0)) return 0;
    if (u >= 1.0) return ((uint64_t)1 << 53) - 1u;
    return (uint64_t)(u * two53);
}

/* -ln(y / 2^53) in Q58 for y in [1, 2^53], within 2^-57 (tested against
 * mpmath at 123 points, and 1,000,000 by tests/compare/trials_jitter_ref.py).
 * y = m 2^e, m in [1, 2); m x INV[i] = 1 + t with i m's next 7 bits and
 * 0 <= t < 2^-7 (INV rounded up); ln(1 + t) by Horner to degree 8, whose
 * truncation is below 2^-70; T[i] = -ln(INV[i]) from mpmath. */
#define YTR__LN2_Q63 0x58b90bfbe8e7bcd6ULL
static const uint64_t ytr__inv_q63[128] = {
    0x8000000000000000ULL, 0x7f01fc07f01fc080ULL, 0x7e07e07e07e07e08ULL, 0x7d1196792909c560ULL,
    0x7c1f07c1f07c1f08ULL, 0x7b301ecc07b301edULL, 0x7a44c6afc2dd9ca9ULL, 0x795ceb240795ceb3ULL,
    0x7878787878787879ULL, 0x77975b8fe21a291dULL, 0x76b981dae6076b99ULL, 0x75ded952e0b0ce46ULL,
    0x7507507507507508ULL, 0x7432d63dbb01d0ccULL, 0x73615a240e6c2b45ULL, 0x7292cc157b864408ULL,
    0x71c71c71c71c71c8ULL, 0x70fe3c070fe3c071ULL, 0x70381c0e070381c1ULL, 0x6f74ae26501bdd2cULL,
    0x6eb3e45306eb3e46ULL, 0x6df5b0f768ce2cacULL, 0x6d3a06d3a06d3a07ULL, 0x6c80d901b2036407ULL,
    0x6bca1af286bca1b0ULL, 0x6b15c06b15c06b16ULL, 0x6a63bd81a98ef607ULL, 0x69b4069b4069b407ULL,
    0x6906906906906907ULL, 0x685b4fe5e92c0686ULL, 0x67b23a5440cf6475ULL, 0x670b453b92840671ULL,
    0x6666666666666667ULL, 0x65c393e032e1c9f1ULL, 0x6522c3f35ba78195ULL, 0x6483ed274388a357ULL,
    0x63e7063e7063e707ULL, 0x634c0634c0634c07ULL, 0x62b2e43dafcea68eULL, 0x621b97c2aec12653ULL,
    0x6186186186186187ULL, 0x60f25deacafb74a4ULL, 0x6060606060606061ULL, 0x5fd017f405fd0180ULL,
    0x5f417d05f417d060ULL, 0x5eb4882383b30d52ULL, 0x5e293205e293205fULL, 0x5d9f7390d2a6c406ULL,
    0x5d1745d1745d1746ULL, 0x5c90a1fd1b7af018ULL, 0x5c0b81702e05c0b9ULL, 0x5b87ddad0cdf1b2dULL,
    0x5b05b05b05b05b06ULL, 0x5a84f3454dca4110ULL, 0x5a05a05a05a05a06ULL, 0x5987b1a9448be406ULL,
    0x590b21642c8590b3ULL, 0x588fe9dc0588fe9eULL, 0x5816058160581606ULL, 0x579d6ee340579d6fULL,
    0x572620ae4c415c99ULL, 0x56b015ac056b015bULL, 0x563b48c20563b48dULL, 0x55c7b4f141ace689ULL,
    0x5555555555555556ULL, 0x54e42523d03fab1cULL, 0x54741fab8be05475ULL, 0x5405405405405406ULL,
    0x5397829cbc14e5e1ULL, 0x532ae21c96bdb9d4ULL, 0x52bf5a814afd6a06ULL, 0x5254e78ecb419ba9ULL,
    0x51eb851eb851eb86ULL, 0x51832f1fd73e6871ULL, 0x511be1958b67ebbaULL, 0x50b59897547e1bbfULL,
    0x5050505050505051ULL, 0x4fec04fec04fec05ULL, 0x4f88b2f392a409f2ULL, 0x4f265691eeaf9d11ULL,
    0x4ec4ec4ec4ec4ec5ULL, 0x4e6470b061fd8cddULL, 0x4e04e04e04e04e05ULL, 0x4da637cf781d1e55ULL,
    0x4d4873ecade304d5ULL, 0x4ceb916d5ef2c784ULL, 0x4c8f8d28ac42fd9cULL, 0x4c346404c346404dULL,
    0x4bda12f684bda130ULL, 0x4b8097012e025c05ULL, 0x4b27ed3604b27ed4ULL, 0x4ad012b404ad012cULL,
    0x4a7904a7904a7905ULL, 0x4a22c04a22c04a23ULL, 0x49cd42e2049cd42fULL, 0x497889c2024bc44fULL,
    0x4924924924924925ULL, 0x48d159e26af37c05ULL, 0x487ede0487ede049ULL, 0x482d1c319f03621eULL,
    0x47dc11f7047dc120ULL, 0x478bbcecfee1d10dULL, 0x473c1ab68a0473c2ULL, 0x46ed29011bb4a405ULL,
    0x469ee58469ee5847ULL, 0x46514e02328a7012ULL, 0x4604604604604605ULL, 0x45b81a2509cde3aeULL,
    0x456c797dd49c3412ULL, 0x45217c382b34eda4ULL, 0x44d72044d72044d8ULL, 0x448d639d74c0cda9ULL,
    0x4444444444444445ULL, 0x43fbc043fbc043fcULL, 0x43b3d5af9a723f79ULL, 0x436c82a23d1a5664ULL,
    0x4325c53ef368eb05ULL, 0x42df9bb096771e4eULL, 0x429a0429a0429a05ULL, 0x4254fce404254fcfULL,
    0x4210842108421085ULL, 0x41cc98291fdf19b4ULL, 0x4189374bc6a7ef9eULL, 0x41465fdf5cd01052ULL,
    0x4104104104104105ULL, 0x40c246d47d78693cULL, 0x4081020408102041ULL, 0x4040404040404041ULL,
};
static const uint64_t ytr__nlinv_q63[128] = {
    0x0000000000000000ULL, 0x00ff015358833c47ULL, 0x01fc0a8b0fc03e3dULL, 0x02f72360fab62355ULL,
    0x03f05361cf066009ULL, 0x04e7a1ee7faf57eaULL, 0x05dd163d8cb73f11ULL, 0x06d0b75c465c6fb0ULL,
    0x07c28c300458a998ULL, 0x08b29b7751bd7073ULL, 0x09a0ebcb0de8e848ULL, 0x0a8d839f830c1fb5ULL,
    0x0b78694572b5a5cdULL, 0x0c61a2eb18cd907aULL, 0x0d49369d256ab1b2ULL, 0x0e2f2a47ade3a18aULL,
    0x0f1383b7157972f4ULL, 0x0ff64898edf55d55ULL, 0x10d77e7cd08e5966ULL, 0x11b72ad52f67a028ULL,
    0x129552f81ff5234bULL, 0x1371fc201e8f743cULL, 0x144d2b6ccb7d1e68ULL, 0x1526e5e3a1b437a3ULL,
    0x15ff3070a793d3c7ULL, 0x16d60fe719d21c8dULL, 0x17ab890210d9091bULL, 0x187fa06520c91090ULL,
    0x19525a9cf456b476ULL, 0x1a23bc1fe2b56319ULL, 0x1af3c94e80bff2d8ULL, 0x1bc286742d8cd62aULL,
    0x1c8ff7c79a9a21abULL, 0x1d5c216b4fbb915aULL, 0x1e27076e2af2e5e9ULL, 0x1ef0adcbdc593651ULL,
    0x1fb9186d5e3e2a8cULL, 0x20804b2969a081b5ULL, 0x214649c4e721c6bfULL, 0x220b17f35c95aa31ULL,
    0x22ceb957574c1c06ULL, 0x23913182d333f5f7ULL, 0x245283f79ef2cb44ULL, 0x2512b427bd0d46bcULL,
    0x25d1c575c23a6137ULL, 0x268fbb3530faa9acULL, 0x274c98aad28bd38dULL, 0x2808610d0d4fc77aULL,
    0x28c3178438bd84faULL, 0x297cbf2aeef141b5ULL, 0x2a355b0e5bf05a12ULL, 0x2aecee2e8ab4d8a8ULL,
    0x2ba37b7eb01394a1ULL, 0x2c5905e5738f2a5eULL, 0x2d0d903d36295d88ULL, 0x2dc11d545743c718ULL,
    0x2e73afed77a00d38ULL, 0x2f254abfba8f44f8ULL, 0x2fd5f077055f86c9ULL, 0x3085a3b43d162e92ULL,
    0x3134670d8284b56bULL, 0x31e23d0e6cc58efcULL, 0x328f2838422df573ULL, 0x333b2b022fc11597ULL,
    0x33e647d97f3097e4ULL, 0x34908121cb761458ULL, 0x3539d93534109319ULL, 0x35e252648ef0d621ULL,
    0x3689eef7991ec519ULL, 0x3730b12d2621f6b3ULL, 0x37d69b3b4e36f7eeULL, 0x387baf4f9b5a9ae7ULL,
    0x391fef8f35344357ULL, 0x39c35e170be7d6f6ULL, 0x3a65fcfc01d7aac2ULL, 0x3b07ce4b145e7ca0ULL,
    0x3ba8d4098389417dULL, 0x3c491034f8d84c39ULL, 0x3ce884c3ad0f0fc6ULL, 0x3d8733a48d19802dULL,
    0x3e251ebf5e0dd967ULL, 0x3ec247f4e05158c2ULL, 0x3f5eb11ef1e63db3ULL, 0x3ffa5c10afe930abULL,
    0x40954a969743fb1aULL, 0x412f7e76a49b4996ULL, 0x41c8f970737d0139ULL, 0x4261bd3d5cd482e3ULL,
    0x42f9cb9094aa0ad9ULL, 0x4391261747322fdbULL, 0x4427ce78b5325bf6ULL, 0x44bdc6564fbef14bULL,
    0x45530f4bd357a6a6ULL, 0x45e7aaef626682ffULL, 0x467b9ad19f25bb30ULL, 0x470ee07dc4f092f1ULL,
    0x47a17d79c10340f7ULL, 0x483373464aadb64cULL, 0x48c4c35efafd0a0bULL, 0x49556f3a63df2cefULL,
    0x49e5784a26c46badULL, 0x4a74dffb0ac22b20ULL, 0x4b03a7b5123a2ee5ULL, 0x4b91d0db9009a100ULL,
    0x4c1f5ccd3c42f87eULL, 0x4cac4ce44875c4c7ULL, 0x4d38a27673874c5bULL, 0x4dc45ed51d1ed716ULL,
    0x4e4f834d58a866a6ULL, 0x4eda1127fff08abdULL, 0x4f6409a9c55bea8eULL, 0x4fed6e1345bd096cULL,
    0x50763fa119cab990ULL, 0x50fe7f8be7399de9ULL, 0x51862f08717b09f3ULL, 0x520d4f47aa237dcdULL,
    0x5293e176c0faec07ULL, 0x5319e6bf33b8e6c4ULL, 0x539f6046dd6ec271ULL, 0x54244f3005a1ad2eULL,
    0x54a8b4996f16abb8ULL, 0x552c919e66525ea4ULL, 0x55afe756cfce6504ULL, 0x5632b6d735e624c1ULL,
    0x56b50130d67cb3ebULL, 0x5736c771b05d9233ULL, 0x57b80aa49059d5c2ULL, 0x5838cbd11e236328ULL,
};

static uint64_t ytr__neglog(uint64_t y) {
    ytr__u128 acc, p;
    uint64_t mq, v, t, s, l1p, sub;
    int e, i, k;
    e = ytr__bitlen(y) - 1;                       /* 0..53 */
    mq = y << (63 - e);                             /* m in Q63, [2^63, 2^64) */
    i = (int)((mq >> 56) & 127u);
    v = ytr__shr(ytr__mul64(mq, ytr__inv_q63[i]), 63);   /* (1 + t) Q63 */
    t = v - ((uint64_t)1 << 63);
    s = ((uint64_t)1 << 63) / 8u;                   /* Horner: 1/8 - t(1/9 ...) */
    for (k = 7; k >= 1; k--)
        s = (((uint64_t)1 << 63) / (uint64_t)k) - ytr__shr(ytr__mul64(t, s), 63);
    l1p = ytr__shr(ytr__mul64(t, s), 63);       /* ln(1 + t) Q63 */
    /* (53 - e) ln 2 - T[i] - ln(1 + t), in Q63 over 128 bits. */
    acc = ytr__mul64((uint64_t)(53 - e), YTR__LN2_Q63);
    sub = ytr__nlinv_q63[i];
    p = acc;
    for (k = 0; k < 2; k++) {
        if (p.lo < sub) {
            if (p.hi == 0) return 0;                /* rounding below 0: -ln(1) */
            p.hi--;
        }
        p.lo -= sub;
        sub = l1p;
    }
    return ytr__shr_round(p, 5);                  /* Q63 -> Q58 */
}

/* The frame whose start, k den / num s, is nearest a duration: its
 * nanoseconds, floor((2 k den 1e9 + num) / (2 num)). */
static int64_t ytr__frame_ns(int64_t k, int num, int den) {
    ytr__u128 x = ytr__mul64(2u * (uint64_t)k, (uint64_t)den * 1000000000u);
    return (int64_t)ytr__div(ytr__add64(x, (uint64_t)num), 2u * (uint64_t)num);
}

/* The first and last frame k whose nanoseconds lie in [lo, hi]. */
static void ytr__frames_in(int64_t lo, int64_t hi, int num, int den, int64_t* klo, int64_t* khi) {
    uint64_t d2 = 2u * (uint64_t)den * 1000000000u;
    ytr__u128 x;
    uint64_t q;
    if (lo <= 0) {
        *klo = 0;
    } else {
        /* k >= (2 lo - 1) num / (2 den 1e9), rounded up */
        x = ytr__mul64(2u * (uint64_t)lo - 1u, (uint64_t)num);
        q = ytr__div(x, d2);
        if (ytr__mul64(q, d2).lo != x.lo || ytr__mul64(q, d2).hi != x.hi) q++;
        *klo = (int64_t)q;
    }
    /* k <= ((2 hi + 1) num - 1) / (2 den 1e9), rounded down */
    x = ytr__mul64(2u * (uint64_t)hi + 1u, (uint64_t)num);
    if (x.lo == 0) x.hi--;
    x.lo--;
    *khi = (int64_t)ytr__div(x, d2);
}

/* Is p drawable? The message names `who` and the first fault. */
static bool ytr__jit_valid(const ytr__jp* p, const char* who, char* err, size_t cap) {
    int i;
    int64_t klo, khi;
    const double mx = YTR_JITTER_MAX_S;
#define YTR__JERR(...) do { if (err && cap) { int n_ = snprintf(err, cap, "%s: ", who); \
        if (n_ >= 0 && (size_t)n_ < cap) snprintf(err + n_, cap - (size_t)n_, __VA_ARGS__); } return false; } while (0)
    if (p->dist < (int)YTR_JITTER_UNIFORM || p->dist > (int)YTR_JITTER_EXPONENTIAL)
        YTR__JERR("dist is not a ytr_jitter_dist");
    if (p->num < 0 || p->den < 0) YTR__JERR("rate_num and rate_den must not be negative");
    if (p->num == 0 && p->den != 0) YTR__JERR("rate_den needs rate_num");
    if (p->dist == (int)YTR_JITTER_CHOICE) {
        if (!p->values || p->n_values < 1 || p->n_values > YTR_MAX_JITTER_VALUES)
            YTR__JERR("CHOICE needs 1 to %d values", YTR_MAX_JITTER_VALUES);
        for (i = 0; i < p->n_values; i++)
            if (!ytr__finite(p->values[i]) || p->values[i] < 0.0 || p->values[i] > mx)
                YTR__JERR("values[%d] must be in [0, %g] s", i, mx);
        return true;
    }
    if (!ytr__finite(p->lo) || !ytr__finite(p->hi) || p->lo < 0.0 || p->hi < p->lo || p->hi > mx)
        YTR__JERR("needs 0 <= lo <= hi <= %g s (lo %g, hi %g)", mx, p->lo, p->hi);
    if (p->dist == (int)YTR_JITTER_EXPONENTIAL &&
        (!ytr__finite(p->scale) || !(p->scale > 0.0) || p->scale > mx || ytr__s_to_ns(p->scale) < 1))
        YTR__JERR("EXPONENTIAL needs a scale in [1 ns, %g s] (scale %g)", mx, p->scale);
    if (p->num > 0) {
        ytr__frames_in(ytr__s_to_ns(p->lo), ytr__s_to_ns(p->hi), p->num, p->den, &klo, &khi);
        if (klo > khi)
            YTR__JERR("no whole frame of %d/%d Hz lies in [%.9g, %.9g] s", p->num, p->den, p->lo, p->hi);
    }
    return true;
#undef YTR__JERR
}

/* The draw for variate u: nanoseconds, or the frame count when p snaps.
 * u53 = floor(u 2^53). UNIFORM: lo + floor(u53 (hi - lo + 1) / 2^53) ns,
 * or that over the frames inside. CHOICE: value floor(u53 n / 2^53).
 * EXPONENTIAL: E = s (-ln(1 - u)), then lo + (E mod (hi - lo)): the
 * exponential is memoryless, so E folded into the interval is exactly the
 * truncated exponential, from one variate and with no exp(); snapped, the
 * same with E counted in frames (a geometric) folded into the frames. */
static int64_t ytr__jit_raw(const ytr__jp* p, double u) {
    uint64_t u53 = ytr__u53(u), n, f, g, q, d;
    int64_t lo, hi, klo, khi, sc;
    int i, sh;
    if (p->dist == (int)YTR_JITTER_CHOICE) {
        i = (int)ytr__shr(ytr__mul64(u53, (uint64_t)p->n_values), 53);
        if (i > p->n_values - 1) i = p->n_values - 1;
        lo = ytr__s_to_ns(p->values[i]);
        if (p->num <= 0) return lo;
        /* the nearest frame: floor((2 v num + den 1e9) / (2 den 1e9)) */
        d = (uint64_t)p->den * 1000000000u;
        return (int64_t)ytr__div(ytr__add64(ytr__mul64(2u * (uint64_t)lo, (uint64_t)p->num), d), 2u * d);
    }
    lo = ytr__s_to_ns(p->lo);
    hi = ytr__s_to_ns(p->hi);
    if (p->num > 0) {
        ytr__frames_in(lo, hi, p->num, p->den, &klo, &khi);
        n = (uint64_t)(khi - klo) + 1u;
        if (p->dist == (int)YTR_JITTER_UNIFORM) return klo + (int64_t)ytr__shr(ytr__mul64(u53, n), 53);
        /* Scale in frames, s num / (den 1e9), as q 2^-sh with q in
         * [2^62, 2^64); then G = floor(-ln(1 - u) x scale in frames). The
         * numerator s num is below 2^81 and den 1e9 at least 2^30, so sh
         * is positive and the shifted numerator fits 128 bits. */
        sc = ytr__s_to_ns(p->scale);
        d = (uint64_t)p->den * 1000000000u;
        {
            ytr__u128 a = ytr__mul64((uint64_t)sc, (uint64_t)p->num);
            int bn = a.hi ? 64 + ytr__bitlen(a.hi) : ytr__bitlen(a.lo);
            sh = 63 + ytr__bitlen(d) - bn;
            q = ytr__div(ytr__shl(a, sh), d);
        }
        f = ytr__neglog(((uint64_t)1 << 53) - u53);
        g = ytr__shr(ytr__mul64(f, q), 58 + sh);
        return klo + (int64_t)(g % n);
    }
    if (p->dist == (int)YTR_JITTER_UNIFORM)
        return lo + (int64_t)ytr__shr(ytr__mul64(u53, (uint64_t)(hi - lo) + 1u), 53);
    sc = ytr__s_to_ns(p->scale);
    f = ytr__neglog(((uint64_t)1 << 53) - u53);
    g = ytr__shr_round(ytr__mul64(f, (uint64_t)sc), 58);   /* E in ns */
    return hi > lo ? lo + (int64_t)(g % (uint64_t)(hi - lo)) : lo;
}

/* Seconds, ns and frames from a stored draw. The seconds are the
 * correctly rounded ns / 1e9: a division (the divisor is volatile so no
 * -freciprocal-math turns it into a product). */
static ytr_jitter_value ytr__jit_value(int64_t raw, int num, int den) {
    static volatile double billion = 1e9;
    ytr_jitter_value v;
    if (num > 0) {
        v.frames = raw;
        v.ns = ytr__frame_ns(raw, num, den);
    } else {
        v.ns = raw;
        v.frames = -1;
    }
    v.s = (double)v.ns / billion;
    return v;
}

static ytr_jitter_value ytr__jit_bad(void) {
    ytr_jitter_value v;
    v.s = YTR__NAN;
    v.ns = -1;
    v.frames = -1;
    return v;
}

/* A desc's own numbers (no columns) as draw parameters, den 0 made 1. */
static void ytr__jit_from_desc(const ytr_jitter_desc* j, ytr__jp* p) {
    p->dist = (int)j->dist;
    p->n_values = j->n_values;
    p->values = j->values;
    p->num = j->rate_num;
    p->den = (j->rate_num > 0 && j->rate_den == 0) ? 1 : j->rate_den;
    p->lo = j->lo;
    p->hi = j->hi;
    p->scale = j->scale;
}

YTR_API bool ytr_jitter_check(const ytr_jitter_desc* j, char* err, size_t cap) {
    ytr__jp p;
    if (err && cap) err[0] = '\0';
    if (!j) {
        if (err && cap) snprintf(err, cap, "ysp_trials: jitter: null desc");
        return false;
    }
    if (j->lo_column || j->hi_column || j->scale_column) {
        if (err && cap) snprintf(err, cap, "ysp_trials: jitter: columns need a session with a table (desc.jitters)");
        return false;
    }
    ytr__jit_from_desc(j, &p);
    return ytr__jit_valid(&p, "ysp_trials: jitter", err, cap);
}

YTR_API ytr_jitter_value ytr_jitter_map(const ytr_jitter_desc* j, double u) {
    ytr__jp p;
    if (!ytr_jitter_check(j, NULL, 0)) return ytr__jit_bad();
    ytr__jit_from_desc(j, &p);
    return ytr__jit_value(ytr__jit_raw(&p, u), p.num, p.den);
}

YTR_API ytr_jitter_value ytr_jitter_draw(const ytr_jitter_desc* j, ytr_rng_fn rng, void* ctx) {
    if (!rng || !ytr_jitter_check(j, NULL, 0)) return ytr__jit_bad();
    {
        ytr__jp p;
        ytr__jit_from_desc(j, &p);
        return ytr__jit_value(ytr__jit_raw(&p, rng(ctx)), p.num, p.den);
    }
}

/* --- text -------------------------------------------------------------- */

/* An appender with snprintf semantics over a whole line built in pieces:
 * `len` keeps counting past `cap`, so the caller learns the size it needs. */
typedef struct ytr__str {
    char*  buf;
    size_t cap;
    size_t len;
} ytr__str;

static void ytr__cat(ytr__str* s, const char* fmt, ...) {
    va_list ap;
    char* dst = NULL;
    size_t room = 0;
    int n;
    if (s->buf && s->len < s->cap) {
        dst = s->buf + s->len;
        room = s->cap - s->len;
    }
    va_start(ap, fmt);
    n = vsnprintf(dst, room, fmt, ap);
    va_end(ap);
    if (n > 0) s->len += (size_t)n;
}

static void ytr__str_init(ytr__str* s, char* buf, size_t cap) {
    s->buf = buf;
    s->cap = buf ? cap : 0;
    s->len = 0;
    if (s->buf && s->cap > 0) s->buf[0] = '\0';
}

static int ytr__str_done(const ytr__str* s) {
    return s->len > 0x7fffffff ? 0x7fffffff : (int)s->len;
}

static const char* ytr__rule_name(ytr_rule r) {
    switch (r) {
    case YTR_RULE_MAX_RUN:       return "max_run";
    case YTR_RULE_MAX_IN_WINDOW: return "max_in_window";
    case YTR_RULE_MIN_GAP:       return "min_gap";
    case YTR_RULE_NO_TRANSITION: return "no_transition";
    case YTR_RULE_FIRST_NOT:     return "first_not";
    case YTR_RULE_FOLLOWED_BY:   return "followed_by";
    case YTR_RULE_PRECEDED_BY:   return "preceded_by";
    case YTR_RULE_CHUNK:         return "chunk";
    case YTR_RULE_BALANCE:       return "balance";
    default:                       return "?";
    }
}

/* A constraint in the form of the helper call that builds it, so a message
 * or a meta line can be pasted back into code. */
static void ytr__describe(ytr__str* s, const ytr_constraint* c) {
    char f[16], l[16];
    if (c->factor == YTR_CONDITION) snprintf(f, sizeof(f), "cond");
    else snprintf(f, sizeof(f), "%d", c->factor);
    if (c->level == YTR_ANY_LEVEL) snprintf(l, sizeof(l), "any");
    else snprintf(l, sizeof(l), "%d", c->level);
    switch (c->rule) {
    case YTR_RULE_MAX_RUN:
    case YTR_RULE_MIN_GAP:
        ytr__cat(s, "%s(%s,%s,%d)", ytr__rule_name(c->rule), f, l, c->n);
        break;
    case YTR_RULE_MAX_IN_WINDOW:
        ytr__cat(s, "%s(%s,%s,%d,%d)", ytr__rule_name(c->rule), f, l, c->window, c->n);
        break;
    case YTR_RULE_NO_TRANSITION:
    case YTR_RULE_FOLLOWED_BY:
    case YTR_RULE_PRECEDED_BY:
        ytr__cat(s, "%s(%s,%s,%d)", ytr__rule_name(c->rule), f, l, c->level2);
        break;
    case YTR_RULE_CHUNK:
        ytr__cat(s, "%s(%s)", ytr__rule_name(c->rule), f);
        break;
    case YTR_RULE_BALANCE:
        ytr__cat(s, "%s(%s,%d)", ytr__rule_name(c->rule), f, c->n);
        break;
    default:
        ytr__cat(s, "%s(%s,%s)", ytr__rule_name(c->rule), f, l);
        break;
    }
}

static bool ytr__fail(ytr_trials* t, const char* fmt, ...) {
    va_list ap;
    int n;
    n = snprintf(t->error, sizeof(t->error), "ysp_trials: ");
    if (n < 0) n = 0;
    va_start(ap, fmt);
    vsnprintf(t->error + n, sizeof(t->error) - (size_t)n, fmt, ap);
    va_end(ap);
    t->open = false;
    return false;
}

/* --- conditions -------------------------------------------------------- */

/* The counts, clamped to the arrays they index. open() and load() already
 * guarantee every count fits its array, so at run time these are the counts
 * themselves. They exist for the optimizer, which cannot see what open()
 * checked: without a bound it can carry a constant from a caller's
 * deliberately bad argument into a path open() makes impossible and then
 * warn about it (gcc 16 does, for loops like these in ysp/quest.h). Every
 * loop and index below that touches a fixed-size array is bounded by one of
 * these, or by the array's size directly. */
static int ytr__clamp(int n, int cap) {
    return (n < 0) ? 0 : (n > cap ? cap : n);
}
static int ytr__nf(const ytr_trials* t) { return ytr__clamp(t->desc.n_factors, YTR_MAX_FACTORS); }
/* Factors as the API counts them: the table's columns, or desc.factors. */
static int ytr__nfe(const ytr_trials* t) { return ytr__clamp(t->n_fac, YTB_MAX_COLUMNS); }
static int ytr__nci(const ytr_trials* t) { return ytr__clamp(t->desc.n_constraints, YTR_MAX_CONSTRAINTS); }
static int ytr__ntr(const ytr_trials* t) { return ytr__clamp(t->desc.n_tracks, YTR_MAX_TRACKS); }
static int ytr__nc(const ytr_trials* t) { return ytr__clamp(t->n_cond, YTR_MAX_CONDITIONS); }
static int ytr__nwl(const ytr_trials* t) { return ytr__clamp(t->desc.n_warmup_conditions, YTR_MAX_CONDITIONS); }
static int ytr__nsch(const ytr_trials* t) { return ytr__clamp(t->n_scheduled, YTR_MAX_TRIALS); }
static int ytr__nrun(const ytr_trials* t) { return ytr__clamp(t->n_run, YTR_MAX_TRIALS); }
static int ytr__nmain(const ytr_trials* t) { return ytr__clamp(t->n_main, YTR_MAX_TRIALS); }

/* Level of factor `f` (or the row, for YTR_CONDITION) in the trial with
 * row `c`; -1 for a track trial, which has no level of anything. */
static int ytr__lv(const ytr_trials* t, int c, int f) {
    if (c < 0) return -1;
    if (f == YTR_CONDITION) return c;
    if (t->desc.table) {
        /* The table's level bytes, little-endian 16-bit per row. */
        const unsigned char* p;
        if (f < 0 || f >= YTB_MAX_COLUMNS || !(p = t->col_lev[f])) return -1;
        return (int)p[2 * c] | (int)p[2 * c + 1] << 8;
    }
    if (f < 0 || f >= YTR_MAX_FACTORS) return -1;
    return (c / t->level_stride[f]) % t->desc.factors[f].n_levels;
}

static int ytr__reps_of(const ytr_trials* t, int c) {
    if (!t->has_cond_reps) return t->desc.reps;
    return (c >= 0 && c < YTR_MAX_CONDITIONS) ? t->cond_reps[c] : 0;
}

/* --- the generator ----------------------------------------------------- */

/* An index below m; see DRAW ORDER. The comparisons are written so a NaN
 * lands on 0 and nothing reaches the (int) conversion out of range. */
static int ytr__draw(ytr_trials* t, int m) {
    double u = t->desc.rng(t->desc.rng_ctx);
    int j;
    if (!(u > 0.0)) return 0;
    if (!(u < 1.0)) return m - 1;
    j = (int)(u * (double)m);
    return j < m ? j : m - 1;
}

static void ytr__shuffle(ytr_trials* t, int16_t* a, int n) {
    int i, j;
    int16_t tmp;
    for (i = n - 1; i > 0; i--) {
        j = ytr__draw(t, i + 1);
        tmp = a[i]; a[i] = a[j]; a[j] = tmp;
    }
}

/* Practice and warmup rows: the k-th from the list in order, or a draw. */
static int ytr__pick_easy(ytr_trials* t, int k) {
    int nl = ytr__nwl(t);
    int m = nl > 0 ? nl : ytr__nc(t);
    int i;
    if (m < 1) return 0;
    i = (t->desc.order == YTR_ORDER_SEQUENTIAL) ? k % m : ytr__draw(t, m);
    return nl > 0 ? t->warm_list[i] : i;
}

/* --- constraints ------------------------------------------------------- */

/* The first constraint the trial at seq[k] breaks, looking back no further
 * than seg (the segment's first index); -1 when it breaks none. Every rule
 * is phrased as "the trial at hand completes a violation", so a scan in
 * order finds the first violated slot and a trial never has to look ahead.
 * The same test serves the repair at open (seq = the main schedule) and the
 * check at run time (seq = the main trials so far plus the candidate). */
static int ytr__violation(const ytr_trials* t, const int16_t* seq, int seg, int k) {
    int ci, j, cnt, lo, own, target, f, nci = ytr__nci(t);
    for (ci = 0; ci < nci; ci++) {
        const ytr_constraint* c = &t->desc.constraints[ci];
        f = c->factor;
        own = ytr__lv(t, seq[k], f);
        if (own < 0) continue;
        target = (c->level == YTR_ANY_LEVEL) ? own : c->level;
        switch (c->rule) {
        case YTR_RULE_MAX_RUN:
            if (own != target) break;
            cnt = 1;
            for (j = k - 1; j >= seg && cnt <= c->n; j--) {
                if (ytr__lv(t, seq[j], f) != target) break;
                cnt++;
            }
            if (cnt > c->n) return ci;
            break;
        case YTR_RULE_MAX_IN_WINDOW:
            if (own != target) break;
            lo = k - c->window + 1;
            if (lo < seg) lo = seg;
            cnt = 0;
            for (j = lo; j <= k; j++)
                if (ytr__lv(t, seq[j], f) == target) cnt++;
            if (cnt > c->n) return ci;
            break;
        case YTR_RULE_MIN_GAP:
            if (own != target) break;
            lo = k - c->n;
            if (lo < seg) lo = seg;
            for (j = lo; j < k; j++)
                if (ytr__lv(t, seq[j], f) == target) return ci;
            break;
        case YTR_RULE_NO_TRANSITION:
            if (own == c->level2 && k - 1 >= seg && ytr__lv(t, seq[k - 1], f) == c->level)
                return ci;
            break;
        case YTR_RULE_FIRST_NOT:
            if (k == 0 && own == c->level) return ci;
            break;
        default:
            break;
        }
    }
    return -1;
}

/* The swap search of CONSTRAINTS, AT OPEN, on seq[0..n). bs > 0 cuts
 * segments every bs slots. Returns -1 when every rule holds, else the
 * constraint still broken, with its slot in *at. */
#define YTR__REPAIR_TRIES 64

static int ytr__repair(ytr_trials* t, int16_t* seq, int n, int bs, int* at) {
    int k = 0, v = -1, j = 0, s, step, tries;
    int16_t tmp;
    bool fits;
    t->swaps = 0;
    for (;;) {
        for (; k < n; k++) {
            v = ytr__violation(t, seq, bs > 0 ? k - k % bs : 0, k);
            if (v >= 0) break;
        }
        if (k >= n) return -1;
        if (t->swaps >= t->desc.max_swaps || n < 2) { *at = k; return v; }
        /* A blind random partner gets trapped at the end of the schedule,
         * where few slots are left to trade with (measured: 15% failures on
         * 3 rows x 10 with no immediate repeat). So the one draw only picks
         * where the search for a partner starts; the first partner that
         * leaves both slots legal looking back wins, and if none of the
         * tries does, the drawn one is taken anyway to move the search on. */
        s = ytr__draw(t, n - 1);
        tries = n - 1 < YTR__REPAIR_TRIES ? n - 1 : YTR__REPAIR_TRIES;
        fits = false;
        for (step = 0; step < tries; step++) {
            j = (k + 1 + (s + step) % (n - 1)) % n;
            if (seq[j] == seq[k]) continue;
            tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp;
            fits = ytr__violation(t, seq, bs > 0 ? k - k % bs : 0, k) < 0 &&
                   ytr__violation(t, seq, bs > 0 ? j - j % bs : 0, j) < 0;
            if (fits) break;
            tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp;
        }
        if (!fits) {
            j = (k + 1 + s) % n;
            tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp;
        }
        t->swaps++;
        /* Only slots at or after the lower of the two changed, and every
         * rule looks back, so the slots before it are still good. */
        if (j < k) k = j;
    }
}

/* --- v0.2: sampling, groups, units, balance ---------------------------- */

/* Slot bits of schedule_flags beside YTR_FLAG_REQUEUED; never copied into
 * the history except LEADIN, which becomes YTR_FLAG_LEADIN. */
#define YTR__SLOT_CONT   16   /* continues the unit of the slot before   */
#define YTR__SLOT_BLOCK  32   /* first slot of a BLOCKED group           */
#define YTR__SLOT_LEADIN 128  /* the lead-in of a balanced order         */

/* Levels of factor f in a desc: the row count for YTR_CONDITION. */
static int ytr__dlev(const ytr_desc* d, int f, int n_cond) {
    if (f == YTR_CONDITION) return n_cond;
    if (d->table) return ytb_n_levels(d->table, f);
    if (f < 0 || f >= YTR_MAX_FACTORS) return 0;
    return d->factors[f].n_levels;
}

static bool ytr__structural(ytr_rule r) {
    return r == YTR_RULE_FOLLOWED_BY || r == YTR_RULE_PRECEDED_BY || r == YTR_RULE_CHUNK;
}

/* Does the desc use anything v0.2 added? Then the snapshot is format 2. */
static bool ytr__is_v2(const ytr_desc* d) {
    int i;
    if (d->table || d->order == YTR_ORDER_LIST || d->order == YTR_ORDER_WITH_REPLACEMENT ||
        d->order_list || d->n_order_list || d->draws || d->weights || d->subset ||
        d->groups.mode != YTR_GROUPS_NONE)
        return true;
    for (i = 0; i < d->n_constraints && i < YTR_MAX_CONSTRAINTS; i++)
        if ((int)d->constraints[i].rule > (int)YTR_RULE_FIRST_NOT) return true;
    return false;
}

/* Does open() need the v0.2 builder (anything but the v0.1 orders)? */
static bool ytr__needs_build2(const ytr_desc* d) {
    int i;
    if (d->order == YTR_ORDER_LIST || d->order == YTR_ORDER_WITH_REPLACEMENT || d->subset ||
        d->groups.mode != YTR_GROUPS_NONE)
        return true;
    for (i = 0; i < d->n_constraints && i < YTR_MAX_CONSTRAINTS; i++)
        if ((int)d->constraints[i].rule > (int)YTR_RULE_FIRST_NOT) return true;
    return false;
}

/* Does next() need the v0.2 run-time path (units, groups, a lead-in)? */
static bool ytr__run2(const ytr_trials* t) {
    return t->units || t->has_leadin || t->desc.groups.mode != YTR_GROUPS_NONE;
}

static int16_t ytr__swap16(int16_t* a, int i, int j) {
    int16_t tmp = a[i];
    a[i] = a[j];
    a[j] = tmp;
    return tmp;
}

/* Fisher-Yates from the top over two parallel arrays (b may be NULL). */
static void ytr__shuffle2(ytr_trials* t, int16_t* a, int16_t* b, int n) {
    int i, j;
    for (i = n - 1; i > 0; i--) {
        j = ytr__draw(t, i + 1);
        (void)ytr__swap16(a, i, j);
        if (b) (void)ytr__swap16(b, i, j);
    }
}

/* Entry j of row `row` of the square (LATIN SQUARES); row in [0, rows). */
static int ytr__latin_at(int n, int row, bool balanced, int j) {
    int r = row, v;
    if (r >= n) {           /* the mirrored half of an odd balanced design */
        r -= n;
        j = n - 1 - j;
    }
    /* Williams' first row: 0, 1, n-1, 2, n-2, 3, ... */
    if (!balanced) v = j;
    else v = (j == 0) ? 0 : (j % 2 == 1 ? (j + 1) / 2 : n - j / 2);
    return (v + r) % n;
}

static int ytr__latin_rows(int n, bool balanced) {
    return (balanced && n % 2 == 1) ? 2 * n : n;
}

YTR_API int ytr_latin(int n, int row, bool balanced, int* out) {
    int rows, j;
    if (n < 1 || n > YTR_MAX_CONDITIONS || row < 0 || !out) return YTR_ERR_ARG;
    rows = ytr__latin_rows(n, balanced);
    for (j = 0; j < n; j++) out[j] = ytr__latin_at(n, row % rows, balanced, j);
    return rows;
}

/* Per-row trial counts (reps or cond_reps) into cnt[], then the subset:
 * a partial Fisher-Yates over the eligible rows in row order picks k of
 * them, the others get 0. Draws: k index draws. */
static bool ytr__counts(ytr_trials* t, int32_t* cnt) {
    int nc = ytr__nc(t), c, m = 0, k = t->desc.subset, i, j;
    int16_t* e = t->work[0];
    /* Under WITH_REPLACEMENT a count only says the row may be drawn. */
    for (c = 0; c < nc; c++)
        cnt[c] = t->desc.order == YTR_ORDER_WITH_REPLACEMENT ? 1 : ytr__reps_of(t, c);
    if (k <= 0) return true;
    for (c = 0; c < nc; c++)
        if (cnt[c] > 0) {
            if (m >= YTR_MAX_TRIALS) return ytr__fail(t, "desc.subset: more rows than YTR_MAX_TRIALS");
            e[m++] = (int16_t)c;
        }
    if (k > m)
        return ytr__fail(t, "desc.subset (%d) is more than the %d rows with trials", k, m);
    for (i = 0; i < k; i++) {
        j = i + ytr__draw(t, m - i);
        (void)ytr__swap16(e, i, j);
    }
    /* tally_valid is zero at open and serves as the "picked" mark. */
    for (i = 0; i < k; i++) t->tally_valid[e[i]] = 1;
    for (c = 0; c < nc; c++)
        if (!t->tally_valid[c]) cnt[c] = 0;
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    return true;
}

/* The trials of rows whose level of factor gf is gl (all rows when gf is
 * -2), repetition-major as SEQUENTIAL lists them, into out[]; under RANDOM
 * each repetition is shuffled as it is written. Returns the count. */
#define YTR__ALL (-2)
static int ytr__trials_of(ytr_trials* t, const int32_t* cnt, int gf, int gl, int16_t* out, int cap,
                            bool shuffle_reps) {
    int nc = ytr__nc(t), c, r, pos = 0, start, maxrep = 0;
    for (c = 0; c < nc; c++)
        if ((gf == YTR__ALL || ytr__lv(t, c, gf) == gl) && cnt[c] > maxrep) maxrep = cnt[c];
    for (r = 0; r < maxrep; r++) {
        start = pos;
        for (c = 0; c < nc && pos < cap; c++)
            if ((gf == YTR__ALL || ytr__lv(t, c, gf) == gl) && cnt[c] > r) out[pos++] = (int16_t)c;
        if (shuffle_reps) ytr__shuffle(t, out + start, pos - start);
    }
    return pos;
}

/* `draws` independent draws by weight among the rows of the part: one raw
 * draw per trial; the row is the first, in row order, whose running sum
 * of weights exceeds u times the total. The running sums are made once
 * (as doubles in the two tally arrays, which are scratch at open) and
 * searched by bisection, which finds that same first row: a linear scan
 * per draw cost 39 ms for 10,000 draws of 10,000 rows. */
static int32_t* ytr__cum_slot(ytr_trials* t, int j) {
    return j < YTR_MAX_CONDITIONS ? &t->tally_valid[j] : &t->tally_pos[j - YTR_MAX_CONDITIONS];
}
static void ytr__cum_set(ytr_trials* t, int c, double v) {
    unsigned char b[8];
    memcpy(b, &v, 8);
    memcpy(ytr__cum_slot(t, 2 * c), b, 4);
    memcpy(ytr__cum_slot(t, 2 * c + 1), b + 4, 4);
}
static double ytr__cum_get(ytr_trials* t, int c) {
    unsigned char b[8];
    double v;
    memcpy(b, ytr__cum_slot(t, 2 * c), 4);
    memcpy(b + 4, ytr__cum_slot(t, 2 * c + 1), 4);
    memcpy(&v, b, 8);
    return v;
}

static bool ytr__draws(ytr_trials* t, const int32_t* cnt, int gf, int gl, const double* w,
                         int16_t* out, int n) {
    int nc = ytr__nc(t), c, k, last = -1, lo, hi, mid;
    double total = 0.0, x;
    /* cnt is tally_pos, which the sums overwrite: keep the counts (0 or 1
     * under WITH_REPLACEMENT) in work[0] and the eligibility in work[1]. */
    if (nc > YTR_MAX_TRIALS) return ytr__fail(t, "draws: more rows than YTR_MAX_TRIALS");
    for (c = 0; c < nc; c++) {
        t->work[0][c] = (int16_t)(cnt[c] > 0 ? 1 : 0);
        t->work[1][c] = (int16_t)(cnt[c] > 0 && (gf == YTR__ALL || ytr__lv(t, c, gf) == gl));
    }
    for (c = 0; c < nc; c++) {
        double wc = t->work[1][c] ? (w ? w[c] : 1.0) : 0.0;
        if (wc > 0.0) {
            total += wc;
            last = c;
        }
        ytr__cum_set(t, c, total);
    }
    if (!(total > 0.0) || last < 0) {
        memset(t->tally_valid, 0, sizeof(t->tally_valid));
        for (c = 0; c < nc; c++) t->tally_pos[c] = t->work[0][c];
        return ytr__fail(t, "the rows to draw from have no weight");
    }
    for (k = 0; k < n; k++) {
        x = t->desc.rng(t->desc.rng_ctx) * total;
        /* The first c with cum[c] > x; none (x >= total, a variate at or
         * above 1) is the last row with weight. */
        lo = 0;
        hi = nc;
        while (lo < hi) {
            mid = lo + (hi - lo) / 2;
            if (ytr__cum_get(t, mid) > x) hi = mid;
            else lo = mid + 1;
        }
        out[k] = (int16_t)(lo < nc ? lo : last);
    }
    /* The counts are needed again for the next group: put them back. */
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    for (c = 0; c < YTR_MAX_CONDITIONS; c++) t->tally_pos[c] = c < nc ? t->work[0][c] : 0;
    return true;
}

/* --- units ------------------------------------------------------------- */

/* The rows a structural rule touches, as a test on row c. */
static bool ytr__touches(const ytr_trials* t, const ytr_constraint* r, int c) {
    int lv = ytr__lv(t, c, r->factor);
    if (r->rule == YTR_RULE_CHUNK) {
        const char* txt = t->desc.table ? ytb_level_text(t->desc.table, r->factor, lv) : NULL;
        return !(txt && txt[0] == '\0');
    }
    return lv == r->level || lv == r->level2;
}

/* Units from the structural rules over the trials seq[0..m) (sequential
 * order), shuffled as units and written back into seq with SLOT_CONT on
 * every trial after a unit's first. Draws: per rule in constraint order,
 * one shuffle of the partner list (followed_by: the second level's trials;
 * preceded_by: the first level's), then one shuffle of the units. */
static bool ytr__make_units(ytr_trials* t, const int32_t* cnt, int16_t* seq, uint8_t* fl, int m) {
    const ytr_desc* d = &t->desc;
    int16_t* nxt = t->work[1];        /* next member, or -1           */
    int16_t* ord = t->work[0];        /* trials in unit order         */
    int16_t* us = t->main_seq;        /* unit starts in ord           */
    int16_t* ul = t->schedule_rep;    /* unit lengths                 */
    int ci, cj, p, q, na, nb, nu = 0, k, nc = ytr__nc(t), c;
    (void)cnt;
    if (m > YTR_MAX_TRIALS) return ytr__fail(t, "too many trials for units");
    for (p = 0; p < m; p++) {
        nxt[p] = -1;
        fl[p] = 0;
    }
    /* Rows touched by two structural rules would join a unit twice. */
    for (ci = 0; ci < ytr__nci(t); ci++) {
        if (!ytr__structural(d->constraints[ci].rule)) continue;
        for (cj = ci + 1; cj < ytr__nci(t); cj++) {
            if (!ytr__structural(d->constraints[cj].rule)) continue;
            for (c = 0; c < nc; c++)
                if (cnt[c] > 0 && ytr__touches(t, &d->constraints[ci], c) &&
                    ytr__touches(t, &d->constraints[cj], c))
                    return ytr__fail(t, "constraints %d and %d both take condition %d into a unit; "
                                       "a longer unit is a chunk column", ci, cj, c);
        }
    }
    for (ci = 0; ci < ytr__nci(t); ci++) {
        const ytr_constraint* r = &d->constraints[ci];
        int16_t* la = us;             /* scratch: first-member positions  */
        int16_t* lb = ul;             /* scratch: second-member positions */
        if (r->rule == YTR_RULE_CHUNK) {
            /* A run's rows share their counts, or a repetition would hold
             * part of a unit; and one value in two separate runs is taken
             * for a typo. */
            int prev = -1, lv, c2;
            for (c = 0; c < nc; c++) {
                if (cnt[c] <= 0) { prev = -1; continue; }
                lv = ytr__touches(t, r, c) ? ytr__lv(t, c, r->factor) : -1;
                if (lv >= 0 && lv != prev) {
                    for (c2 = 0; c2 < c; c2++)
                        if (cnt[c2] > 0 && ytr__touches(t, r, c2) && ytr__lv(t, c2, r->factor) == lv)
                            return ytr__fail(t, "constraint %d, chunk: rows %d and %d have one value but are "
                                               "not contiguous", ci, c2, c);
                }
                if (lv >= 0 && lv == prev && cnt[c] != cnt[c - 1])
                    return ytr__fail(t, "constraint %d, chunk: rows %d and %d are one unit with different "
                                       "repetitions", ci, c - 1, c);
                prev = lv;
            }
            /* Contiguous rows with one non-empty value, per repetition: in
             * the sequential order they are contiguous trials too. */
            for (p = 0; p + 1 < m; p++) {
                int a = seq[p], b = seq[p + 1];
                if (b == a + 1 && ytr__touches(t, r, a) &&
                    ytr__lv(t, a, r->factor) == ytr__lv(t, b, r->factor)) {
                    nxt[p] = (int16_t)(p + 1);
                    fl[p + 1] = 1;
                }
            }
            continue;
        }
        if (r->rule != YTR_RULE_FOLLOWED_BY && r->rule != YTR_RULE_PRECEDED_BY) continue;
        na = nb = 0;
        for (p = 0; p < m; p++) {
            int lv = ytr__lv(t, seq[p], r->factor);
            if (lv == r->level) la[na++] = (int16_t)p;
            else if (lv == r->level2) lb[nb++] = (int16_t)p;
        }
        if (r->rule == YTR_RULE_FOLLOWED_BY) {
            /* Each `level` trial gets a distinct `level2` trial after it. */
            if (nb < na)
                return ytr__fail(t, "constraint %d, followed_by: %d trials need a follower and only %d "
                                   "can follow", ci, na, nb);
            ytr__shuffle2(t, lb, NULL, nb);
            for (k = 0; k < na; k++) {
                nxt[la[k]] = lb[k];
                fl[lb[k]] = 1;
            }
        } else {
            /* Each `level` trial gets a distinct `level2` trial before it. */
            if (nb < na)
                return ytr__fail(t, "constraint %d, preceded_by: %d trials need a predecessor and only "
                                   "%d can precede", ci, na, nb);
            ytr__shuffle2(t, lb, NULL, nb);
            for (k = 0; k < na; k++) {
                nxt[lb[k]] = la[k];
                fl[la[k]] = 1;
            }
        }
    }
    /* Units in order of their first member; chunk runs chain through nxt. */
    q = 0;
    for (p = 0; p < m; p++) {
        int x, len = 0;
        if (fl[p]) continue;
        us[nu] = (int16_t)q;
        for (x = p; x >= 0; x = nxt[x]) {
            if (q >= m) return ytr__fail(t, "internal: a unit loops");
            ord[q++] = seq[x];
            len++;
        }
        ul[nu++] = (int16_t)len;
    }
    ytr__shuffle2(t, us, ul, nu);
    p = 0;
    for (k = 0; k < nu; k++) {
        int j;
        for (j = 0; j < ul[k]; j++) {
            seq[p] = ord[us[k] + j];
            fl[p] = (uint8_t)(j > 0 ? YTR__SLOT_CONT : 0);
            p++;
        }
    }
    t->units = true;
    return true;
}

/* Move the unit at slots [u, u + len) so that it starts at slot `to`
 * (to < u: the slots between shift up; to >= u + len: they shift down and
 * the unit ends just before `to`). Returns the unit's new first slot. */
static int ytr__move_unit(ytr_trials* t, int16_t* seq, uint8_t* fl, int u, int len, int to) {
    int16_t* tmp = t->work[0];
    int16_t* tmpf = t->work[1];
    int j;
    for (j = 0; j < len; j++) {
        tmp[j] = seq[u + j];
        tmpf[j] = fl[u + j];
    }
    if (to < u) {
        memmove(seq + to + len, seq + to, (size_t)(unsigned)(u - to) * sizeof(seq[0]));
        memmove(fl + to + len, fl + to, (size_t)(unsigned)(u - to) * sizeof(fl[0]));
    } else {
        memmove(seq + u, seq + u + len, (size_t)(unsigned)(to - u - len) * sizeof(seq[0]));
        memmove(fl + u, fl + u + len, (size_t)(unsigned)(to - u - len) * sizeof(fl[0]));
        to -= len;
    }
    for (j = 0; j < len; j++) {
        seq[to + j] = tmp[j];
        fl[to + j] = (uint8_t)tmpf[j];
    }
    return to;
}

static int ytr__unit_len(const uint8_t* fl, int k, int hi) {
    int e = k + 1;
    while (e < hi && (fl[e] & YTR__SLOT_CONT)) e++;
    return e - k;
}

/* The repair for a design with units, on seq[lo, hi): scan for the first
 * slot that breaks a rule; MOVE the unit holding it to another unit
 * boundary, the first of up to 64 from a random start (one index draw per
 * move, below the number of units minus 1) after which the moved trials
 * and the slot that closes the gap obey every rule looking back; when none
 * does, the drawn boundary is taken anyway. Rescan from the lowest slot that
 * changed. Moves, not swaps: a swap cannot change a sequence whose
 * length-1 units are all alike (measured, docs/trials.md). Segments are
 * never cut: the schedule must hold across every boundary, because a block
 * boundary inside a unit moves to the unit's end. */
static int ytr__repair_units(ytr_trials* t, int16_t* seq, uint8_t* fl, int lo, int hi, int* at) {
    int16_t* ust = t->main_seq;   /* unit start slots */
    int k = lo, v = -1, nu, ui, s, step, tries, b, q, u, len, nw, g, x, lowest;
    bool fits;
    t->swaps = 0;
    for (;;) {
        for (; k < hi; k++) {
            v = ytr__violation(t, seq, lo, k);
            if (v >= 0) break;
        }
        if (k >= hi) return -1;
        nu = 0;
        for (x = lo; x < hi; x++)
            if (!(fl[x] & YTR__SLOT_CONT)) ust[nu++] = (int16_t)x;
        if (t->swaps >= t->desc.max_swaps || nu < 2) { *at = k; return v; }
        u = k;
        while (u > lo && (fl[u] & YTR__SLOT_CONT)) u--;
        len = ytr__unit_len(fl, u, hi);
        for (ui = 0; ui < nu && ust[ui] != u; ui++) {}
        s = ytr__draw(t, nu - 1);
        tries = nu - 1 < YTR__REPAIR_TRIES ? nu - 1 : YTR__REPAIR_TRIES;
        fits = false;
        for (step = 0; step <= tries && !fits; step++) {
            /* Boundary b of the sequence without the unit (0..nu-1, the
             * unit's own place is b == ui); the pass after the tries takes
             * the drawn boundary whether it fits or not. */
            b = (ui + 1 + (step < tries ? (s + step) % (nu - 1) : s)) % nu;
            if (b == ui) {
                if (step < tries) continue;
                b = (b + 1) % nu;
            }
            x = b < ui ? b : b + 1;       /* as an index of the full sequence */
            q = x < nu ? ust[x] : hi;
            nw = ytr__move_unit(t, seq, fl, u, len, q);
            g = q < u ? u + len : u;      /* the slot that closes the gap */
            lowest = nw < u ? nw : u;
            if (step < tries) {
                fits = true;
                for (x = nw; x <= nw + len && x < hi && fits; x++)
                    if (ytr__violation(t, seq, lo, x) >= 0) fits = false;
                if (fits && g < hi && ytr__violation(t, seq, lo, g) >= 0) fits = false;
                if (!fits) {
                    (void)ytr__move_unit(t, seq, fl, nw, len, q < u ? u + len : u);
                    continue;
                }
            }
            fits = true;
            if (lowest < k) k = lowest;
        }
        t->swaps++;
    }
}

/* The v0.1 swap repair on seq[lo, hi) with one more condition on a
 * partner: when pf != YTR__ALL, it must have the same level of factor pf
 * (the balanced factor, or the group factor of ALTERNATE groups), so the
 * swap keeps every transition or the group cycle. bs > 0 cuts segments
 * every bs slots from lo. When none of the tries fits, the first partner
 * at or after the drawn one that has the same level is taken anyway. */
static int ytr__repair_pred(ytr_trials* t, int16_t* seq, int lo, int hi, int bs, int pf, int* at) {
    int n = hi - lo, k = lo, v = -1, j = 0, s, step, tries, kr;
    int16_t tmp;
    bool fits;
    t->swaps = 0;
#define YTR__SEG(x) (bs > 0 ? (x) - ((x) - lo) % bs : lo)
    for (;;) {
        for (; k < hi; k++) {
            v = ytr__violation(t, seq, YTR__SEG(k), k);
            if (v >= 0) break;
        }
        if (k >= hi) return -1;
        if (t->swaps >= t->desc.max_swaps || n < 2) { *at = k; return v; }
        kr = k - lo;
        s = ytr__draw(t, n - 1);
        tries = n - 1 < YTR__REPAIR_TRIES ? n - 1 : YTR__REPAIR_TRIES;
        fits = false;
        for (step = 0; step < tries; step++) {
            j = lo + (kr + 1 + (s + step) % (n - 1)) % n;
            if (seq[j] == seq[k]) continue;
            if (pf != YTR__ALL && ytr__lv(t, seq[j], pf) != ytr__lv(t, seq[k], pf)) continue;
            tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp;
            fits = ytr__violation(t, seq, YTR__SEG(k), k) < 0 &&
                   ytr__violation(t, seq, YTR__SEG(j), j) < 0;
            if (fits) break;
            tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp;
        }
        if (!fits) {
            /* The first partner at or after the drawn one that keeps the
             * predicate; none means no swap this round. */
            int st;
            for (st = 0; st < n - 1; st++) {
                j = lo + (kr + 1 + (s + st) % (n - 1)) % n;
                if (pf == YTR__ALL || ytr__lv(t, seq[j], pf) == ytr__lv(t, seq[k], pf)) break;
            }
            if (st < n - 1) { tmp = seq[k]; seq[k] = seq[j]; seq[j] = tmp; }
            else j = k;
        }
        t->swaps++;
        if (j < k) k = j;
    }
#undef YTR__SEG
}

/* A repair's failure message, as v0.1 words it. */
static bool ytr__repair_fail(ytr_trials* t, int v, int at) {
    ytr__str s;
    char what[96];
    ytr__str_init(&s, what, sizeof(what));
    ytr__describe(&s, &t->desc.constraints[v]);
    return ytr__fail(t, "constraint %d, %s, is still broken at main slot %d after %d swaps; "
                       "the design may be impossible (raise desc.max_swaps if not)",
                       v, what, at, t->swaps);
}

/* --- balance ------------------------------------------------------------ */

/* A uniform random sequence in which every ordered pair of the levels of
 * factor f (that have trials) is adjacent equally often: an Euler circuit
 * of the complete directed graph on those levels, arcs lambda times, loops
 * unless NO_REPEAT, drawn by Kandel, Matias, Unger and Winkler (1996) as
 * Brooks (2012) recommends. Writes the main schedule into seq (the lead-in
 * first, unless NO_LEADIN) and returns its length, or -1. Draws, in order:
 * the start level (an index draw below n), the backward walk (an index
 * draw below the number of predecessors per step), each level's arc order
 * (Fisher-Yates over the arcs that are not its tree arc, in level order),
 * each level's trials (Fisher-Yates, in level order), the lead-in's trial
 * (an index draw below the level's trial count). */
static int ytr__balance(ytr_trials* t, const int32_t* cnt, const ytr_constraint* rule, int16_t* seq,
                          uint8_t* fl, int cap) {
    int f = rule->factor, nlev, n = 0, c, l, per, lam, c0 = -1, e, u, w, j, s, cur, seen, p, deg, v;
    bool loops = !(rule->n & YTR_BALANCE_NO_REPEAT), leadin = !(rule->n & YTR_BALANCE_NO_LEADIN);
    int16_t* vlev = t->aux;           /* vertex -> level                  */
    int32_t* lc = t->tally_valid;     /* trials per level (scratch)       */
    int16_t* arcs = t->work[0];       /* arc targets, deg per vertex      */
    int16_t* tw = t->work[1];         /* tree, visited, next-arc cursor    */
    int16_t* walk = t->main_seq;      /* the vertex sequence              */
    int nc = ytr__nc(t);
    nlev = ytr__dlev(&t->desc, f, nc);
    if (nlev > YTR_MAX_CONDITIONS) nlev = YTR_MAX_CONDITIONS;
    for (l = 0; l < nlev; l++) lc[l] = 0;
    for (c = 0; c < nc; c++) {
        l = ytr__lv(t, c, f);
        if (l >= 0 && l < nlev) lc[l] += cnt[c];
    }
    for (l = 0; l < nlev; l++) {
        if (lc[l] == 0) continue;
        if (c0 < 0) c0 = lc[l];
        if (lc[l] != c0) {
            ytr__fail(t, "balance: every level needs the same number of trials; level %d has %d, "
                        "level %d has %d", (int)vlev[0], c0, l, lc[l]);
            memset(lc, 0, sizeof(int32_t) * (size_t)nlev);
            return -1;
        }
        vlev[n++] = (int16_t)l;
    }
    memset(lc, 0, sizeof(int32_t) * (size_t)nlev);
    per = loops ? n : n - 1;
    if (n < 1 || per < 1 || c0 % per != 0) {
        ytr__fail(t, "balance: %d levels with %d trials each; each level needs a multiple of %d "
                    "trials (n%s x lambda)", n, c0 < 0 ? 0 : c0, per < 1 ? 1 : per, loops ? "" : " - 1");
        return -1;
    }
    lam = c0 / per;
    deg = c0;
    e = n * deg;
    if (e + (leadin ? 1 : 0) > cap || 3 * n > YTR_MAX_TRIALS) {
        ytr__fail(t, "balance: the order does not fit YTR_MAX_TRIALS");
        return -1;
    }
    (void)lam;
    /* Arcs of vertex u: every target v (not u without loops), lambda times. */
    for (u = 0; u < n; u++) {
        p = 0;
        for (v = 0; v < n; v++) {
            if (!loops && v == u) continue;
            for (j = 0; j < c0 / per; j++) arcs[u * deg + p++] = (int16_t)v;
        }
    }
    /* 1. start, 2. backward walk: the first entry into w records w's tree
     * arc, w -> where the walk came from. tw[0..n) tree, tw[n..2n) seen. */
    s = ytr__draw(t, n);
    for (u = 0; u < n; u++) { tw[u] = -1; tw[n + u] = 0; }
    tw[n + s] = 1;
    seen = 1;
    cur = s;
    while (seen < n) {
        j = ytr__draw(t, loops ? n : n - 1);
        w = loops ? j : (j >= cur ? j + 1 : j);
        if (!tw[n + w]) {
            tw[n + w] = 1;
            tw[w] = (int16_t)cur;
            seen++;
        }
        cur = w;
    }
    /* 3. Each vertex's arc order: shuffled, the tree arc last. */
    for (u = 0; u < n; u++) {
        int16_t* a = arcs + u * deg;
        if (u != s) {
            for (j = 0; j < deg; j++)
                if (a[j] == tw[u]) break;
            (void)ytr__swap16(a, j, deg - 1);
            ytr__shuffle2(t, a, NULL, deg - 1);
        } else {
            ytr__shuffle2(t, a, NULL, deg);
        }
    }
    /* 4. Walk from s, each vertex's next unused arc. */
    for (u = 0; u < n; u++) tw[2 * n + u] = 0;
    walk[0] = (int16_t)s;
    cur = s;
    for (j = 1; j <= e; j++) {
        /* Separate statements: cur is both the index of the cursor that
         * moves and the value assigned, an unsequenced pair in one
         * expression (MSVC and gcc disagreed on it). */
        int at = tw[2 * n + cur];
        tw[2 * n + cur] = (int16_t)(at + 1);
        cur = arcs[cur * deg + at];
        walk[j] = (int16_t)cur;
    }
    /* 5. Trials into their level's places: the level's trials in
     * repetition-major order, shuffled. */
    p = leadin ? 1 : 0;
    for (u = 0; u < n; u++) {
        int k = 0, maxrep = 0, r;
        for (c = 0; c < nc; c++)
            if (ytr__lv(t, c, f) == vlev[u] && cnt[c] > maxrep) maxrep = cnt[c];
        for (r = 0; r < maxrep; r++)
            for (c = 0; c < nc; c++)
                if (ytr__lv(t, c, f) == vlev[u] && cnt[c] > r && k < YTR_MAX_TRIALS) arcs[k++] = (int16_t)c;
        ytr__shuffle2(t, arcs, NULL, k);
        k = 0;
        for (j = 1; j <= e; j++)
            if (walk[j] == u) seq[p + j - 1] = arcs[k++];
    }
    for (j = 0; j < e + p; j++) fl[j] = 0;
    if (leadin) {
        /* The lead-in: one of the start level's trials, again. */
        int k = 0, maxrep = 0, r, want;
        for (c = 0; c < nc; c++)
            if (ytr__lv(t, c, f) == vlev[s] && cnt[c] > maxrep) maxrep = cnt[c];
        want = ytr__draw(t, c0);
        seq[0] = 0;
        for (r = 0; r < maxrep; r++)
            for (c = 0; c < nc; c++)
                if (ytr__lv(t, c, f) == vlev[s] && cnt[c] > r) {
                    if (k == want) seq[0] = (int16_t)c;
                    k++;
                }
        fl[0] = YTR__SLOT_LEADIN;
        t->has_leadin = true;
    }
    return e + p;
}

/* --- the v0.2 builder ---------------------------------------------------- */

/* Any rule the repair checks (the v0.1 five)? */
static bool ytr__has_rules(const ytr_trials* t) {
    int ci;
    for (ci = 0; ci < ytr__nci(t); ci++)
        if ((int)t->desc.constraints[ci].rule <= (int)YTR_RULE_FIRST_NOT) return true;
    return false;
}

/* One part (the whole design, or one group): its trials in the inner
 * order, units, repair. seq/fl point at the part's first slot of the main
 * schedule (base), lo is that slot's main index. Returns the length, -1 on
 * failure. */
static int ytr__part(ytr_trials* t, const int32_t* cnt, int gf, int gl, int16_t* base, uint8_t* bfl,
                       int lo, int cap, bool repair) {
    const ytr_desc* d = &t->desc;
    int16_t* seq = base + lo;
    uint8_t* fl = bfl + lo;
    int m, ci, v, at = -1;
    bool has_struct = false;
    if (d->order == YTR_ORDER_WITH_REPLACEMENT) {
        m = d->draws;
        if (m > cap) { ytr__fail(t, "the draws do not fit YTR_MAX_TRIALS"); return -1; }
        if (!ytr__draws(t, cnt, gf, gl, d->weights, seq, m)) return -1;
        memset(fl, 0, (size_t)m);
        return m;
    }
    m = ytr__trials_of(t, cnt, gf, gl, seq, cap, d->order == YTR_ORDER_RANDOM);
    memset(fl, 0, (size_t)m);
    if (d->order == YTR_ORDER_SEQUENTIAL || d->order == YTR_ORDER_RANDOM) return m;
    for (ci = 0; ci < ytr__nci(t); ci++)
        if (ytr__structural(d->constraints[ci].rule)) has_struct = true;
    if (d->order == YTR_ORDER_CONSTRAINED && has_struct) {
        if (!ytr__make_units(t, cnt, seq, fl, m)) return -1;
    } else {
        ytr__shuffle(t, seq, m);
    }
    if (d->order != YTR_ORDER_CONSTRAINED || !repair || m == 0 || !ytr__has_rules(t)) return m;
    if (has_struct) {
        v = ytr__repair_units(t, base, bfl, lo, lo + m, &at);
    } else {
        int bs = (gf == YTR__ALL && !d->constraints_span_blocks) ? d->block_size : 0;
        v = ytr__repair_pred(t, base, lo, lo + m, bs, YTR__ALL, &at);
    }
    if (v >= 0 && v < YTR_MAX_CONSTRAINTS && d->n_tracks == 0) {
        ytr__repair_fail(t, v, at);
        return -1;
    }
    return m;
}

/* The groups' run order: the group levels that have trials, into aux[]
 * (where setup left groups.list for LIST). Returns the count, or -1.
 * Draws: RANDOM, one shuffle of the present levels in level order. */
static int ytr__group_order(ytr_trials* t, const int32_t* cnt) {
    const ytr_group_desc* g = &t->desc.groups;
    int nc = ytr__nc(t), nlev = ytr__dlev(&t->desc, g->factor, nc), c, l, n = 0, i, rows;
    int32_t* present = t->tally_valid;    /* zero at open */
    int16_t* out = t->work[1];
    if (nlev > YTR_MAX_CONDITIONS || nlev > YTR_MAX_TRIALS) {
        ytr__fail(t, "groups: the factor has too many levels");
        return -1;
    }
    for (c = 0; c < nc; c++) {
        if (t->desc.order == YTR_ORDER_WITH_REPLACEMENT) {
            double w = t->desc.weights ? t->desc.weights[c] : 1.0;
            if (cnt[c] <= 0 || !(w > 0.0)) continue;
        } else if (cnt[c] <= 0) {
            continue;
        }
        l = ytr__lv(t, c, g->factor);
        if (l >= 0 && l < nlev) present[l] = 1;
    }
    switch (g->order) {
    case YTR_GROUP_ORDER_RANDOM:
        for (l = 0; l < nlev; l++) if (present[l]) out[n++] = (int16_t)l;
        ytr__shuffle2(t, out, NULL, n);
        break;
    case YTR_GROUP_ORDER_LATIN:
    case YTR_GROUP_ORDER_BALANCED_LATIN: {
        bool bal = g->order == YTR_GROUP_ORDER_BALANCED_LATIN;
        rows = ytr__latin_rows(nlev, bal);
        for (i = 0; i < nlev; i++) {
            l = ytr__latin_at(nlev, g->participant % rows, bal, i);
            if (present[l]) out[n++] = (int16_t)l;
        }
        break;
    }
    case YTR_GROUP_ORDER_LIST:
        for (i = 0; i < g->n_list && i < nlev; i++) {
            l = t->aux[i];
            if (present[l] == 1) { out[n++] = (int16_t)l; present[l] = 2; }
        }
        for (l = 0; l < nlev; l++)
            if (present[l] == 1) {
                memset(present, 0, sizeof(int32_t) * (size_t)nlev);
                ytr__fail(t, "groups: level %d has trials and is not in groups.list", l);
                return -1;
            }
        break;
    default:
        for (l = 0; l < nlev; l++) if (present[l]) out[n++] = (int16_t)l;
        break;
    }
    memset(present, 0, sizeof(int32_t) * (size_t)nlev);
    for (i = 0; i < n; i++) t->aux[i] = out[i];
    return n;
}

static bool ytr__build2(ytr_trials* t) {
    const ytr_desc* d = &t->desc;
    int np = ytr__clamp(d->n_practice, YTR_MAX_TRIALS);
    int cap = YTR_MAX_TRIALS - np, m = 0, k, c, ci, ng, gi;
    int32_t* cnt = t->tally_pos;      /* zero at open; the per-row counts here */
    int16_t* base = t->schedule + np;
    uint8_t* bfl = t->schedule_flags + np;
    const ytr_constraint* bal = NULL;
    int16_t glist[1];
    (void)glist;

    for (k = 0; k < np; k++) {
        t->schedule[k] = (int16_t)ytr__pick_easy(t, k);
        t->schedule_rep[k] = -1;
    }
    if (d->order == YTR_ORDER_LIST) {
        /* Setup kept the list in work[0]. */
        for (k = 0; k < d->n_order_list && k < cap; k++) {
            base[k] = t->work[0][k];
            bfl[k] = 0;
        }
        m = k;
    } else {
        if (!ytr__counts(t, cnt)) goto fail;
        for (ci = 0; ci < ytr__nci(t); ci++)
            if (d->constraints[ci].rule == YTR_RULE_BALANCE) bal = &d->constraints[ci];
        if (d->groups.mode == YTR_GROUPS_NONE) {
            if (bal) {
                m = ytr__balance(t, cnt, bal, base, bfl, cap);
                if (m < 0) goto fail;
                for (ci = 0, k = 0; ci < ytr__nci(t); ci++)
                    if ((int)d->constraints[ci].rule <= (int)YTR_RULE_FIRST_NOT) k = 1;
                if (k) {
                    int at = -1, v;
                    int bs = d->constraints_span_blocks ? 0 : d->block_size;
                    v = ytr__repair_pred(t, base, 0, m, bs, bal->factor, &at);
                    if (v >= 0 && v < YTR_MAX_CONSTRAINTS) { ytr__repair_fail(t, v, at); goto fail; }
                }
            } else {
                m = ytr__part(t, cnt, YTR__ALL, 0, base, bfl, 0, cap, true);
                if (m < 0) goto fail;
            }
        } else {
            int gf = d->groups.factor;
            ng = ytr__group_order(t, cnt);
            if (ng < 0) goto fail;
            if (d->groups.mode == YTR_GROUPS_BLOCKED) {
                for (gi = 0; gi < ng; gi++) {
                    int len = ytr__part(t, cnt, gf, t->aux[gi], base, bfl, m, cap - m, false);
                    if (len < 0) goto fail;
                    if (d->order == YTR_ORDER_CONSTRAINED && len > 0) {
                        int at = -1, v = -1;
                        for (ci = 0, k = 0; ci < ytr__nci(t); ci++)
                            if ((int)d->constraints[ci].rule <= (int)YTR_RULE_FIRST_NOT) k = 1;
                        if (k) {
                            v = t->units ? ytr__repair_units(t, base, bfl, m, m + len, &at)
                                         : ytr__repair_pred(t, base, m, m + len, 0, YTR__ALL, &at);
                            if (v >= 0 && v < YTR_MAX_CONSTRAINTS) { ytr__repair_fail(t, v, at); goto fail; }
                        }
                    }
                    if (len > 0) bfl[m] |= YTR__SLOT_BLOCK;
                    m += len;
                }
            } else {
                /* ALTERNATE: every group in level order, then one trial of
                 * each in run order, cycling. */
                int16_t* gl = t->schedule_rep;   /* group level -> start */
                int16_t* tmp = t->work[0];
                int size = -1, nlev = ytr__dlev(d, gf, ytr__nc(t)), l, done;
                int16_t* order = t->aux;
                for (l = 0; l < nlev && l < YTR_MAX_TRIALS; l++) gl[l] = -1;
                done = 0;
                for (l = 0; l < nlev; l++) {
                    for (gi = 0; gi < ng; gi++) if (order[gi] == l) break;
                    if (gi >= ng) continue;
                    gl[l] = (int16_t)done;
                    k = ytr__part(t, cnt, gf, l, base, bfl, done, cap - done, false);
                    if (k < 0) goto fail;
                    if (size >= 0 && k != size) {
                        ytr__fail(t, "groups: ALTERNATE needs groups of one size; group level %d has %d "
                                    "trials, another has %d", l, k, size);
                        goto fail;
                    }
                    size = k;
                    done += k;
                }
                m = done;
                for (k = 0; k < m; k++) tmp[k] = base[k];
                for (k = 0; k < (size > 0 ? size : 0); k++)
                    for (gi = 0; gi < ng; gi++) base[k * ng + gi] = tmp[gl[order[gi]] + k];
                for (k = 0; k < m; k++) bfl[k] = 0;
                if (d->order == YTR_ORDER_CONSTRAINED) {
                    int at = -1, v;
                    int bs = d->constraints_span_blocks ? 0 : d->block_size;
                    v = ytr__repair_pred(t, base, 0, m, bs, gf, &at);
                    if (v >= 0 && v < YTR_MAX_CONSTRAINTS) { ytr__repair_fail(t, v, at); goto fail; }
                }
            }
        }
    }
    t->n_scheduled = np + m;
    /* Reps number each row's main slots from 0; a lead-in has none. */
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    for (k = 0; k < m; k++) {
        c = base[k];
        if (bfl[k] & YTR__SLOT_LEADIN) { t->schedule_rep[np + k] = -1; continue; }
        if (c < 0 || c >= YTR_MAX_CONDITIONS) continue;
        t->schedule_rep[np + k] = (int16_t)t->tally_valid[c]++;
    }
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    memset(t->tally_pos, 0, sizeof(t->tally_pos));
    memset(t->main_seq, 0, sizeof(t->main_seq));
    return true;
fail:
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    memset(t->tally_pos, 0, sizeof(t->tally_pos));
    return false;
}

/* --- lifecycle --------------------------------------------------------- */

static bool ytr__check_constraint(ytr_trials* t, const ytr_desc* d, int ci, int n_cond, int n_fac) {
    const ytr_constraint* c = &d->constraints[ci];
    int n_lev;
    bool any_ok;
    if ((int)c->rule < (int)YTR_RULE_MAX_RUN || (int)c->rule > (int)YTR_RULE_BALANCE)
        return ytr__fail(t, "desc.constraints[%d].rule is not a ytr_rule", ci);
    if (c->factor != YTR_CONDITION && (c->factor < 0 || c->factor >= n_fac))
        return ytr__fail(t, "desc.constraints[%d].factor is not a factor or YTR_CONDITION", ci);
    n_lev = ytr__dlev(d, c->factor, n_cond);
    if (c->rule == YTR_RULE_CHUNK) {
        if (c->factor == YTR_CONDITION)
            return ytr__fail(t, "desc.constraints[%d]: chunk needs a factor, not YTR_CONDITION", ci);
        return true;
    }
    if (c->rule == YTR_RULE_BALANCE) {
        if (c->n & ~(YTR_BALANCE_NO_REPEAT | YTR_BALANCE_NO_LEADIN))
            return ytr__fail(t, "desc.constraints[%d]: balance takes only YTR_BALANCE_* flags in n", ci);
        return true;
    }
    any_ok = c->rule == YTR_RULE_MAX_RUN || c->rule == YTR_RULE_MAX_IN_WINDOW ||
             c->rule == YTR_RULE_MIN_GAP;
    if (c->level == YTR_ANY_LEVEL) {
        if (!any_ok)
            return ytr__fail(t, "desc.constraints[%d]: %s takes no YTR_ANY_LEVEL", ci,
                               ytr__rule_name(c->rule));
    } else if (c->level < 0 || c->level >= n_lev) {
        return ytr__fail(t, "desc.constraints[%d].level is out of range", ci);
    }
    switch (c->rule) {
    case YTR_RULE_MAX_RUN:
    case YTR_RULE_MIN_GAP:
        if (c->n < 1) return ytr__fail(t, "desc.constraints[%d].n must be at least 1", ci);
        break;
    case YTR_RULE_MAX_IN_WINDOW:
        if (c->n < 1 || c->window < 1)
            return ytr__fail(t, "desc.constraints[%d]: window and count must be at least 1", ci);
        break;
    case YTR_RULE_NO_TRANSITION:
        if (c->level2 < 0 || c->level2 >= n_lev)
            return ytr__fail(t, "desc.constraints[%d].level2 is out of range", ci);
        break;
    case YTR_RULE_FOLLOWED_BY:
    case YTR_RULE_PRECEDED_BY:
        if (c->level2 < 0 || c->level2 >= n_lev)
            return ytr__fail(t, "desc.constraints[%d].level2 is out of range", ci);
        if (c->level2 == c->level)
            return ytr__fail(t, "desc.constraints[%d]: %s needs two different levels", ci,
                               ytr__rule_name(c->rule));
        break;
    default:
        break;
    }
    return true;
}

/* FNV-1a 64 over v0.2 arrays the handle does not keep (weights, the order
 * list, the group list), so a snapshot can check that load() was given the
 * same ones. */
static uint64_t ytr__mix_in(uint64_t h, uint64_t v) {
    int i;
    for (i = 0; i < 8; i++) {
        h ^= (v >> (8 * i)) & 0xffu;
        h *= 0x100000001B3ULL;
    }
    return h;
}

/* Everything open() and load() share: validate, copy the desc with its
 * defaults resolved, size the schedule. No generator draw and no track
 * call, so load() can use it. */
/* --- jitter in a session ------------------------------------------------- */

/* Draw parameters of jitter j for a trial of condition `cond` (-1 for a
 * track trial): the jitter's own numbers, with any column's value for the
 * row in their place. */
static void ytr__jit_params(const ytr_trials* t, int j, int cond, ytr__jp* p) {
    const ytr__jit* q = &t->jit[j];
    const ytb_table* tb = t->desc.table;
    p->dist = q->dist;
    p->n_values = q->n_values;
    p->values = q->values;
    p->num = q->rate_num;
    p->den = q->rate_den;
    p->lo = q->lo;
    p->hi = q->hi;
    p->scale = q->scale;
    if (cond >= 0 && tb) {
        if (q->col[0] >= 0) p->lo = ytb_num(tb, cond, q->col[0]);
        if (q->col[1] >= 0) p->hi = ytb_num(tb, cond, q->col[1]);
        if (q->col[2] >= 0) p->scale = ytb_num(tb, cond, q->col[2]);
    }
}

static bool ytr__jit_name_ok(const char* nm) {
    size_t i, n;
    if (!nm) return false;
    n = strlen(nm);
    if (n < 1 || n > YTR_MAX_JITTER_NAME) return false;
    for (i = 0; i < n; i++) {
        char c = nm[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || (i > 0 && c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/* Copy desc.jitters[j] into the handle, resolve its columns, and check it
 * for every row that can use it (and for track trials, which use the
 * jitter's own numbers). n_cond and the table are set. */
static bool ytr__jit_setup(ytr_trials* t, const ytr_desc* d, int j, int n_cond) {
    const ytr_jitter_desc* s = &d->jitters[j];
    ytr__jit* q = &t->jit[j];
    const char* cols[3];
    char who[96], msg[256];
    ytr__jp p;
    int k, c;
    bool any_col = false;
    if (!ytr__jit_name_ok(s->name))
        return ytr__fail(t, "desc.jitters[%d].name must be 1 to %d of [A-Za-z0-9_], not starting with a digit",
                           j, YTR_MAX_JITTER_NAME);
    for (k = 0; k < j; k++)
        if (strcmp(t->jit[k].name, s->name) == 0)
            return ytr__fail(t, "desc.jitters[%d] and [%d] are both named '%s'", k, j, s->name);
    memcpy(q->name, s->name, strlen(s->name) + 1);
    snprintf(who, sizeof(who), "desc.jitters[%d] (%s)", j, q->name);
    ytr__jit_from_desc(s, &p);
    q->dist = p.dist;
    q->rate_num = p.num;
    q->rate_den = p.den;
    q->lo = p.lo;
    q->hi = p.hi;
    q->scale = p.scale;
    q->n_values = 0;
    if (p.dist == (int)YTR_JITTER_CHOICE && p.values && p.n_values >= 1 && p.n_values <= YTR_MAX_JITTER_VALUES) {
        for (k = 0; k < p.n_values; k++) q->values[k] = p.values[k];
        q->n_values = p.n_values;
    }
    cols[0] = s->lo_column;
    cols[1] = s->hi_column;
    cols[2] = s->scale_column;
    for (k = 0; k < 3; k++) {
        q->col[k] = -1;
        if (!cols[k]) continue;
        if (!d->table) return ytr__fail(t, "%s: a column needs desc.table", who);
        if (p.dist == (int)YTR_JITTER_CHOICE) return ytr__fail(t, "%s: a CHOICE jitter takes no columns", who);
        if (k == 2 && p.dist != (int)YTR_JITTER_EXPONENTIAL)
            return ytr__fail(t, "%s: scale_column needs an EXPONENTIAL jitter", who);
        q->col[k] = ytb_col(d->table, cols[k]);
        if (q->col[k] < 0) return ytr__fail(t, "%s: the table has no column '%s'", who, cols[k]);
        if (ytb_col_type(d->table, q->col[k]) == YTB_STRING)
            return ytr__fail(t, "%s: column '%s' is not numeric", who, cols[k]);
        any_col = true;
    }
    /* With columns the jitter's own numbers are used only by track trials. */
    if (!any_col || d->n_tracks > 0) {
        if (!ytr__jit_valid(&p, who, msg, sizeof(msg))) return ytr__fail(t, "%s", msg);
    }
    if (any_col) {
        for (c = 0; c < n_cond; c++) {
            ytr__jit_params(t, j, c, &p);
            snprintf(who, sizeof(who), "desc.jitters[%d] (%s), row %d", j, q->name, c);
            if (!ytr__jit_valid(&p, who, msg, sizeof(msg))) return ytr__fail(t, "%s", msg);
        }
    }
    return true;
}

/* One variate per jitter, in jitter order, for the trial just pushed. */
static void ytr__jit_draw(ytr_trials* t, int i, int cond) {
    ytr__jp p;
    int j;
    for (j = 0; j < t->n_jit && j < YTR_MAX_JITTERS; j++) {
        ytr__jit_params(t, j, cond, &p);
        t->jit_raw[i][j] = ytr__jit_raw(&p, t->desc.rng(t->desc.rng_ctx));
    }
}

static bool ytr__setup(ytr_trials* t, const ytr_desc* d) {
    int i, f, n_cond, n_main = 0, n_fac, n_struct = 0, n_groups = 1;
    long prod;
    const char* why = NULL;
    const ytr_constraint* bal = NULL;
    uint64_t h = 0xCBF29CE484222325ULL;

    memset(t, 0, sizeof(*t));
    t->current = -1;
    t->blk = -1;
    t->balance_ci = -1;
    if (!d) return ytr__fail(t, "null desc");

    if (d->n_conditions < 0) return ytr__fail(t, "desc.n_conditions is negative");
    if (d->n_factors < 0 || d->n_factors > YTR_MAX_FACTORS)
        return ytr__fail(t, "desc.n_factors must be in [0, YTR_MAX_FACTORS]");
    n_cond = d->n_conditions;
    n_fac = d->n_factors;
    if (d->table) {
        const ytb_table* tb = d->table;
        if (!tb->base || tb->n_cols < 1 || tb->n_cols > YTB_MAX_COLUMNS)
            return ytr__fail(t, "desc.table is not a table from ytb_csv() or ytb_view()");
        if (d->n_factors != 0) return ytr__fail(t, "desc.table and desc.factors cannot both be given");
        if (tb->n_rows < 1) return ytr__fail(t, "desc.table has no rows");
        if (tb->n_rows > YTR_MAX_CONDITIONS)
            return ytr__fail(t, "desc.table has %d rows, above YTR_MAX_CONDITIONS (%d); define "
                               "YTR_MAX_CONDITIONS larger before including ysp/trials.h",
                               tb->n_rows, YTR_MAX_CONDITIONS);
        if (d->n_conditions != 0 && d->n_conditions != tb->n_rows)
            return ytr__fail(t, "desc.n_conditions is not 0 or the table's row count");
        n_cond = tb->n_rows;
        n_fac = tb->n_cols;
    } else if (d->n_factors > 0) {
        prod = 1;
        for (f = 0; f < d->n_factors && f < YTR_MAX_FACTORS; f++) {
            if (d->factors[f].n_levels < 1)
                return ytr__fail(t, "desc.factors[%d].n_levels must be at least 1", f);
            if (d->factors[f].n_levels > YTR_MAX_CONDITIONS)
                return ytr__fail(t, "the factors' product is above YTR_MAX_CONDITIONS");
            prod *= d->factors[f].n_levels;
            if (prod > YTR_MAX_CONDITIONS)
                return ytr__fail(t, "the factors' product is above YTR_MAX_CONDITIONS");
        }
        if (d->n_conditions != 0 && d->n_conditions != (int)prod)
            return ytr__fail(t, "desc.n_conditions is not 0 or the factors' product");
        n_cond = (int)prod;
    }
    if (n_cond > YTR_MAX_CONDITIONS)
        return ytr__fail(t, "desc.n_conditions is above YTR_MAX_CONDITIONS");
    /* Levels must be readable while the rest is checked. */
    t->desc = *d;
    t->n_cond = n_cond;
    t->n_fac = n_fac;
    if (d->table)
        for (f = 0; f < n_fac && f < YTB_MAX_COLUMNS; f++) t->col_lev[f] = ytb_level_bytes(d->table, f);
    f = ytr__nf(t);
    if (f > 0) {
        t->level_stride[f - 1] = 1;
        for (f = f - 2; f >= 0; f--)
            t->level_stride[f] = t->level_stride[f + 1] * t->desc.factors[f + 1].n_levels;
    }

    if (d->n_tracks < 0 || d->n_tracks > YTR_MAX_TRACKS)
        return ytr__fail(t, "desc.n_tracks must be in [0, YTR_MAX_TRACKS]");
    if (n_cond == 0 && d->n_tracks == 0)
        return ytr__fail(t, "desc needs conditions (n_conditions or factors) or tracks");
    if ((int)d->order < (int)YTR_ORDER_SEQUENTIAL || (int)d->order > (int)YTR_ORDER_WITH_REPLACEMENT)
        return ytr__fail(t, "desc.order is not a ytr_order");
    if ((int)d->interleave < (int)YTR_INTERLEAVE_RANDOM ||
        (int)d->interleave > (int)YTR_INTERLEAVE_ROUND_ROBIN)
        return ytr__fail(t, "desc.interleave is not a ytr_interleave");
    if (d->subset < 0) return ytr__fail(t, "desc.subset is negative");
    if (d->order != YTR_ORDER_LIST && (d->order_list || d->n_order_list))
        return ytr__fail(t, "desc.order_list needs desc.order = YTR_ORDER_LIST");
    if (d->order != YTR_ORDER_WITH_REPLACEMENT && (d->draws || d->weights))
        return ytr__fail(t, "desc.draws and desc.weights need desc.order = YTR_ORDER_WITH_REPLACEMENT");
    if ((int)d->groups.mode < (int)YTR_GROUPS_NONE || (int)d->groups.mode > (int)YTR_GROUPS_ALTERNATE)
        return ytr__fail(t, "desc.groups.mode is not a ytr_group_mode");
    if (n_cond == 0 && (d->order == YTR_ORDER_LIST || d->order == YTR_ORDER_WITH_REPLACEMENT ||
                        d->subset || d->groups.mode != YTR_GROUPS_NONE))
        return ytr__fail(t, "an order list, draws, a subset or groups need conditions");

    if (n_cond > 0) {
        if (d->order == YTR_ORDER_LIST) {
            if (d->reps != 0 || d->cond_reps || d->subset)
                return ytr__fail(t, "YTR_ORDER_LIST takes no reps, cond_reps or subset: the list is the schedule");
            if (!d->order_list || d->n_order_list < 1)
                return ytr__fail(t, "YTR_ORDER_LIST needs desc.order_list and desc.n_order_list >= 1");
            if (d->n_order_list > YTR_MAX_TRIALS)
                return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
            for (i = 0; i < d->n_order_list && i < YTR_MAX_TRIALS; i++) {
                int v = d->order_list[i];
                if (v < 0 || v >= n_cond) return ytr__fail(t, "desc.order_list[%d] is not a condition", i);
                t->work[0][i] = (int16_t)v;
                h = ytr__mix_in(h, (uint64_t)(uint32_t)v);
            }
            n_main = d->n_order_list;
        } else if (d->order == YTR_ORDER_WITH_REPLACEMENT) {
            double sum = 0.0;
            if (d->reps != 0 || d->cond_reps)
                return ytr__fail(t, "YTR_ORDER_WITH_REPLACEMENT takes no reps or cond_reps; desc.draws is the count");
            if (d->draws < 1) return ytr__fail(t, "YTR_ORDER_WITH_REPLACEMENT needs desc.draws >= 1");
            if (d->draws > YTR_MAX_TRIALS) return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
            if (d->subset > n_cond)
                return ytr__fail(t, "desc.subset (%d) is more than the %d rows with trials", d->subset, n_cond);
            for (i = 0; i < n_cond; i++) {
                double w = d->weights ? d->weights[i] : 1.0;
                uint64_t u;
                if (!ytr__finite(w) || w < 0.0)
                    return ytr__fail(t, "desc.weights[%d] must be finite and not negative", i);
                sum += w;
                memcpy(&u, &w, sizeof(u));
                if (d->weights) h = ytr__mix_in(h, u);
            }
            if (!(sum > 0.0)) return ytr__fail(t, "desc.weights sum to 0");
            n_main = d->draws;
        } else if (d->cond_reps) {
            /* Read the caller's array exactly once, n_cond entries, into the
             * handle; nothing reads it again. */
            int rc;
            for (i = 0; i < n_cond && i < YTR_MAX_CONDITIONS; i++) {
                rc = d->cond_reps[i];
                if (rc < 0)
                    return ytr__fail(t, "desc.cond_reps[%d] is negative", i);
                if (rc > YTR_MAX_TRIALS)
                    return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
                t->cond_reps[i] = (int16_t)rc;
                n_main += rc;
                if (n_main > YTR_MAX_TRIALS && !d->subset)
                    return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
            }
            if (n_main < 1) return ytr__fail(t, "desc.cond_reps sums to 0");
            t->has_cond_reps = true;
        } else {
            if (d->reps < 1)
                return ytr__fail(t, "desc.reps must be at least 1 (or give desc.cond_reps)");
            if (d->reps > YTR_MAX_TRIALS ||
                ((long)d->reps * n_cond > YTR_MAX_TRIALS && !d->subset))
                return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
            n_main = d->reps * n_cond;
        }
        if (d->subset > 0 && d->order != YTR_ORDER_WITH_REPLACEMENT && d->order != YTR_ORDER_LIST) {
            /* The most trials a subset of k rows can give: the k largest
             * counts, from a histogram of the counts (work[0]; a count is
             * at most YTR_MAX_TRIALS, which gets its own counter). */
            int m = 0, k, c, v, top = 0;
            for (c = 0; c < YTR_MAX_TRIALS; c++) t->work[0][c] = 0;
            for (c = 0; c < n_cond; c++) {
                v = ytr__reps_of(t, c);
                if (v <= 0) continue;
                m++;
                if (v >= YTR_MAX_TRIALS) top++;
                else t->work[0][v]++;
            }
            if (d->subset > m)
                return ytr__fail(t, "desc.subset (%d) is more than the %d rows with trials", d->subset, m);
            k = d->subset;
            n_main = 0;
            v = top < k ? top : k;
            n_main += v * YTR_MAX_TRIALS;
            k -= v;
            for (c = YTR_MAX_TRIALS - 1; c > 0 && k > 0; c--) {
                v = t->work[0][c] < k ? t->work[0][c] : k;
                n_main += v * c;
                k -= v;
            }
            if (n_main > YTR_MAX_TRIALS) return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
        }
    }

    if (d->n_constraints < 0 || d->n_constraints > YTR_MAX_CONSTRAINTS)
        return ytr__fail(t, "desc.n_constraints must be in [0, YTR_MAX_CONSTRAINTS]");
    if (d->n_constraints > 0 && d->order != YTR_ORDER_CONSTRAINED)
        return ytr__fail(t, "constraints need desc.order = YTR_ORDER_CONSTRAINED");
    for (i = 0; i < d->n_constraints && i < YTR_MAX_CONSTRAINTS; i++) {
        if (!ytr__check_constraint(t, d, i, n_cond, n_fac)) return false;
        if (ytr__structural(d->constraints[i].rule)) n_struct++;
        if (d->constraints[i].rule == YTR_RULE_BALANCE) {
            if (bal) return ytr__fail(t, "desc.constraints[%d]: one balance rule at most", i);
            bal = &d->constraints[i];
            t->balance_ci = i;
        }
    }
    if (bal) {
        if (d->n_tracks > 0) return ytr__fail(t, "balance needs no tracks: track trials would cut its transitions");
        if (d->groups.mode != YTR_GROUPS_NONE) return ytr__fail(t, "balance and groups cannot be combined");
        if (n_struct > 0) return ytr__fail(t, "balance and structural rules cannot be combined");
        for (i = 0; i < d->n_constraints && i < YTR_MAX_CONSTRAINTS; i++) {
            const ytr_constraint* c = &d->constraints[i];
            if (c == bal) continue;
            if (c->factor == bal->factor || bal->factor == YTR_CONDITION)
                return ytr__fail(t, "desc.constraints[%d] constrains the balanced factor, which only "
                                   "balance_no_repeat can (a repair would break the balance)", i);
        }
        if (!(bal->n & YTR_BALANCE_NO_LEADIN)) n_main++;
    }
    if (n_struct > 0 && d->subset) return ytr__fail(t, "structural rules and desc.subset cannot be combined");
    if (d->max_swaps < 0) return ytr__fail(t, "desc.max_swaps is negative");

    if (d->groups.mode != YTR_GROUPS_NONE) {
        const ytr_group_desc* g = &d->groups;
        int nlev;
        if (g->factor < 0 || g->factor >= n_fac) return ytr__fail(t, "desc.groups.factor is not a factor");
        nlev = ytr__dlev(d, g->factor, n_cond);
        if ((int)g->order < (int)YTR_GROUP_ORDER_SEQUENTIAL || (int)g->order > (int)YTR_GROUP_ORDER_LIST)
            return ytr__fail(t, "desc.groups.order is not a ytr_group_order");
        if (g->participant < 0) return ytr__fail(t, "desc.groups.participant is negative");
        if (d->n_tracks > 0)
            return ytr__fail(t, "groups need no tracks (one handle per block is the way to mix them)");
        if (d->order == YTR_ORDER_LIST) return ytr__fail(t, "groups and YTR_ORDER_LIST cannot be combined");
        if (g->mode == YTR_GROUPS_BLOCKED && d->block_size != 0)
            return ytr__fail(t, "BLOCKED groups are the blocks: desc.block_size must be 0");
        if (g->mode == YTR_GROUPS_ALTERNATE && n_struct > 0)
            return ytr__fail(t, "ALTERNATE groups and structural rules cannot be combined");
        if (g->mode == YTR_GROUPS_ALTERNATE && d->order == YTR_ORDER_WITH_REPLACEMENT)
            return ytr__fail(t, "ALTERNATE groups take no WITH_REPLACEMENT order");
        if (g->order == YTR_GROUP_ORDER_LIST) {
            if (!g->list || g->n_list < 1 || g->n_list > nlev)
                return ytr__fail(t, "desc.groups.list needs 1 to %d group levels", nlev);
            for (i = 0; i < g->n_list && i < YTR_MAX_CONDITIONS; i++) {
                int v = g->list[i], k;
                if (v < 0 || v >= nlev) return ytr__fail(t, "desc.groups.list[%d] is not a level", i);
                for (k = 0; k < i; k++)
                    if (t->aux[k] == v) return ytr__fail(t, "desc.groups.list[%d] repeats level %d", i, v);
                t->aux[i] = (int16_t)v;
                h = ytr__mix_in(h, (uint64_t)(uint32_t)v);
            }
        }
        /* Groups that will have trials: with WITH_REPLACEMENT, each draws
         * `draws` times. tally_valid is scratch. */
        if (g->mode == YTR_GROUPS_BLOCKED && d->order == YTR_ORDER_WITH_REPLACEMENT) {
            int c, l;
            n_groups = 0;
            for (c = 0; c < n_cond; c++) {
                double w = d->weights ? d->weights[c] : 1.0;
                l = ytr__lv(t, c, g->factor);
                if (w > 0.0 && l >= 0 && l < YTR_MAX_CONDITIONS && !t->tally_valid[l]) {
                    t->tally_valid[l] = 1;
                    n_groups++;
                }
            }
            memset(t->tally_valid, 0, sizeof(t->tally_valid));
            if ((long)n_groups * d->draws > YTR_MAX_TRIALS)
                return ytr__fail(t, "more scheduled trials than YTR_MAX_TRIALS");
            n_main = n_groups * d->draws;
        }
        if (g->mode == YTR_GROUPS_BLOCKED) n_groups = nlev;
    }

    for (i = 0; i < d->n_tracks && i < YTR_MAX_TRACKS; i++) {
        if (!d->tracks[i].is_done)
            return ytr__fail(t, "desc.tracks[%d].is_done is NULL", i);
        if (!ytr__finite(d->tracks[i].weight) || d->tracks[i].weight < 0.0)
            return ytr__fail(t, "desc.tracks[%d].weight must be finite and not negative", i);
    }
    if (!ytr__finite(d->track_rate) || d->track_rate < 0.0 || d->track_rate > 1.0)
        return ytr__fail(t, "desc.track_rate must be in [0, 1]");
    if (d->n_tracks > 0 && n_cond > 0 && d->track_rate == 0.0)
        return ytr__fail(t, "desc.track_rate is required with both conditions and tracks");

    if (d->block_size < 0) return ytr__fail(t, "desc.block_size is negative");
    if (d->n_practice < 0 || d->n_warmup < 0)
        return ytr__fail(t, "desc.n_practice and desc.n_warmup must not be negative");
    if (d->n_practice > YTR_MAX_TRIALS || d->n_warmup > YTR_MAX_TRIALS)
        return ytr__fail(t, "more practice or warmup trials than YTR_MAX_TRIALS");
    if ((d->n_practice > 0 || d->n_warmup > 0) && n_cond == 0)
        return ytr__fail(t, "practice and warmup trials need conditions");
    if (d->n_warmup > 0 && d->block_size == 0 && d->groups.mode != YTR_GROUPS_BLOCKED)
        return ytr__fail(t, "desc.n_warmup needs desc.block_size");
    if (d->n_warmup_conditions < 0)
        return ytr__fail(t, "desc.n_warmup_conditions is negative");
    if (d->n_warmup_conditions > YTR_MAX_CONDITIONS)
        return ytr__fail(t, "desc.n_warmup_conditions is above YTR_MAX_CONDITIONS");
    if (d->n_warmup_conditions > 0) {
        int wc;
        if (!d->warmup_conditions)
            return ytr__fail(t, "desc.n_warmup_conditions without desc.warmup_conditions");
        for (i = 0; i < d->n_warmup_conditions && i < YTR_MAX_CONDITIONS; i++) {
            wc = d->warmup_conditions[i];
            if (wc < 0 || wc >= n_cond)
                return ytr__fail(t, "desc.warmup_conditions[%d] is not a condition", i);
            t->warm_list[i] = (int16_t)wc;
        }
    }
    if (d->requeue_gap < 0) return ytr__fail(t, "desc.requeue_gap is negative");
    if (d->record_size > 0 && !d->records)
        return ytr__fail(t, "desc.record_size needs desc.records");
    if (d->n_jitters < 0 || d->n_jitters > YTR_MAX_JITTERS)
        return ytr__fail(t, "desc.n_jitters must be in [0, YTR_MAX_JITTERS (%d)]", YTR_MAX_JITTERS);
    for (i = 0; i < d->n_jitters && i < YTR_MAX_JITTERS; i++)
        if (!ytr__jit_setup(t, d, i, n_cond)) return false;

    /* Practice and warmup draw only under an order other than SEQUENTIAL,
     * and they need conditions, so the first test covers them. */
    if (!d->rng) {
        if (n_cond > 0 && d->order != YTR_ORDER_SEQUENTIAL && d->order != YTR_ORDER_LIST)
            why = "an order other than SEQUENTIAL (and its practice and warmup draws)";
        else if (d->order == YTR_ORDER_LIST && (d->n_practice > 0 || d->n_warmup > 0))
            why = "practice and warmup draws under YTR_ORDER_LIST";
        else if (d->subset > 0)
            why = "desc.subset";
        else if (d->groups.mode != YTR_GROUPS_NONE && d->groups.order == YTR_GROUP_ORDER_RANDOM)
            why = "a RANDOM group order";
        else if (d->n_tracks > 1 && d->interleave == YTR_INTERLEAVE_RANDOM)
            why = "RANDOM interleave of more than one track";
        else if (d->n_tracks > 0 && n_cond > 0 && d->track_rate < 1.0)
            why = "a track_rate below 1";
        else if (d->requeue_gap > 0)
            why = "a requeue_gap";
        else if (d->n_jitters > 0)
            why = "jitter draws";
        if (why) return ytr__fail(t, "desc.rng is required for %s", why);
    }

    {
        long total = (long)d->n_practice + n_main;
        if (d->n_tracks == 0 && d->n_warmup > 0 && n_main > 0) {
            long blocks = 1;
            if (d->groups.mode == YTR_GROUPS_BLOCKED) blocks = n_groups;
            else if (d->block_size > 0) blocks = ((long)n_main + d->block_size - 1) / d->block_size;
            total += (blocks - 1) * (long)d->n_warmup;
        }
        if (total > YTR_MAX_TRIALS)
            return ytr__fail(t, "practice, scheduled and warmup trials exceed YTR_MAX_TRIALS (%ld > %d)",
                               total, YTR_MAX_TRIALS);
    }

    t->desc = *d;
    /* The handle holds copies; clearing the pointers makes any later read
     * of the caller's arrays a crash in testing, not a silent dependency.
     * The v0.2 arrays are read at open and summarized in aux_hash. */
    t->desc.cond_reps = NULL;
    t->desc.warmup_conditions = NULL;
    t->desc.order_list = NULL;
    t->desc.groups.list = NULL;
    /* Jitters live in t->jit; the desc's copies would dangle. */
    memset(t->desc.jitters, 0, sizeof(t->desc.jitters));
    t->n_jit = d->n_jitters;
    t->aux_hash = h;
    t->had_weights = d->weights != NULL;
    t->v2 = ytr__is_v2(d);
    if (t->desc.max_swaps == 0) t->desc.max_swaps = YTR__DEFAULT_SWAPS;
    if (n_cond == 0) t->desc.track_rate = 1.0;
    else if (d->n_tracks == 0) t->desc.track_rate = 0.0;
    for (i = 0; i < ytr__ntr(t); i++)
        if (t->desc.tracks[i].weight == 0.0) t->desc.tracks[i].weight = 1.0;
    if (n_cond == 0) {
        t->desc.reps = 0;
        t->has_cond_reps = false;
    }
    t->blk_next = d->block_size;
    t->n_scheduled = d->n_practice + n_main;
    return true;
}

/* Fill the schedule: practice draws, the order, the repair, the reps. The
 * draws happen in exactly the order RANDOMNESS promises. */
static bool ytr__build(ytr_trials* t) {
    const ytr_desc* d = &t->desc;
    int np = ytr__clamp(d->n_practice, YTR_MAX_TRIALS);
    int n_main = ytr__clamp(ytr__nsch(t) - np, YTR_MAX_TRIALS - np);
    int nc = ytr__nc(t);
    int k, c, r, maxrep = 0, pos, start, at = -1, v;
    int16_t* main_sched = t->schedule + np;

    if (ytr__needs_build2(d)) return ytr__build2(t);
    for (k = 0; k < np; k++) {
        t->schedule[k] = (int16_t)ytr__pick_easy(t, k);
        t->schedule_rep[k] = -1;
    }

    for (c = 0; c < nc; c++)
        if (ytr__reps_of(t, c) > maxrep) maxrep = ytr__reps_of(t, c);
    pos = np;
    for (r = 0; r < maxrep; r++) {
        start = pos;
        for (c = 0; c < nc && pos < np + n_main; c++)
            if (ytr__reps_of(t, c) > r) t->schedule[pos++] = (int16_t)c;
        if (d->order == YTR_ORDER_RANDOM) ytr__shuffle(t, t->schedule + start, pos - start);
    }
    if (d->order == YTR_ORDER_FULL_RANDOM || d->order == YTR_ORDER_CONSTRAINED)
        ytr__shuffle(t, main_sched, n_main);

    if (d->order == YTR_ORDER_CONSTRAINED && d->n_constraints > 0 && n_main > 0) {
        int bs = (d->n_tracks == 0 && !d->constraints_span_blocks) ? d->block_size : 0;
        v = ytr__repair(t, main_sched, n_main, bs, &at);
        if (v >= 0 && v < YTR_MAX_CONSTRAINTS && d->n_tracks == 0) {
            ytr__str s;
            char what[96];
            ytr__str_init(&s, what, sizeof(what));
            ytr__describe(&s, &d->constraints[v]);
            return ytr__fail(t, "constraint %d, %s, is still broken at main slot %d after %d swaps; "
                               "the design may be impossible (raise desc.max_swaps if not)",
                               v, what, at, t->swaps);
        }
    }

    /* The tallies are zero here and serve as the per-row counter. */
    for (k = 0; k < n_main; k++) {
        c = main_sched[k];
        if (c < 0 || c >= YTR_MAX_CONDITIONS) continue;
        t->schedule_rep[np + k] = (int16_t)t->tally_valid[c]++;
    }
    memset(t->tally_valid, 0, sizeof(t->tally_valid));
    return true;
}

YTR_API bool ytr_open(ytr_trials* t, const ytr_desc* desc) {
    if (!t) return false;
    if (!ytr__setup(t, desc)) return false;
    if (!ytr__build(t)) return false;
    t->desc.weights = NULL;     /* read at open, summarized in aux_hash */
    t->error[0] = '\0';
    t->open = true;
    return true;
}

YTR_API const char* ytr_error(const ytr_trials* t) {
    return t ? t->error : "ysp_trials: null handle";
}

YTR_API bool ytr_is_open(const ytr_trials* t) {
    return t != NULL && t->open;
}

/* --- conditions -------------------------------------------------------- */

YTR_API int ytr_n_conditions(const ytr_trials* t) {
    return (t && t->open) ? t->n_cond : 0;
}

YTR_API int ytr_n_factors(const ytr_trials* t) {
    return (t && t->open) ? t->n_fac : 0;
}

YTR_API int ytr_level(const ytr_trials* t, int condition, int factor) {
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (condition < 0 || condition >= t->n_cond) return YTR_ERR_ARG;
    if (factor != YTR_CONDITION && (factor < 0 || factor >= ytr__nfe(t)))
        return YTR_ERR_ARG;
    return ytr__lv(t, condition, factor);
}

YTR_API int ytr_condition_from_levels(const ytr_trials* t, const int* levels) {
    int f, c = 0;
    if (!t || !levels) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (t->desc.table) {
        /* Rows need not be a product: the first row with these levels. */
        for (c = 0; c < ytr__nc(t); c++) {
            for (f = 0; f < ytr__nfe(t); f++)
                if (ytr__lv(t, c, f) != levels[f]) break;
            if (f == ytr__nfe(t)) return c;
        }
        return YTR_ERR_ARG;
    }
    if (ytr__nf(t) == 0) return YTR_ERR_ARG;
    for (f = 0; f < ytr__nf(t); f++) {
        if (levels[f] < 0 || levels[f] >= t->desc.factors[f].n_levels) return YTR_ERR_ARG;
        c += levels[f] * t->level_stride[f];
    }
    return c;
}

YTR_API int ytr_condition_at(const ytr_trials* t, int i) {
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (i < 0 || i >= t->n_scheduled) return YTR_ERR_ARG;
    return t->schedule[i];
}

YTR_API int ytr_n_scheduled(const ytr_trials* t) {
    return (t && t->open) ? t->n_scheduled : 0;
}

/* --- the loop ---------------------------------------------------------- */

static void ytr__info(const ytr_trials* t, int i, ytr_trial_info* info) {
    const ytr_trial* h = &t->history[i];
    if (!info) return;
    info->index = i;
    info->condition = h->condition;
    info->track = h->track;
    info->rep = h->rep;
    info->block = h->block;
    info->first_in_block = (h->flags & YTR_FLAG_FIRST_IN_BLOCK) != 0;
    info->after_break = (h->flags & YTR_FLAG_AFTER_BREAK) != 0;
    info->practice = (h->flags & YTR_FLAG_PRACTICE) != 0;
    info->warmup = (h->flags & YTR_FLAG_WARMUP) != 0;
    info->requeued = (h->flags & YTR_FLAG_REQUEUED) != 0;
}

static int ytr__push(ytr_trials* t, int cond, int track, int rep, int block, int flags,
                       ytr_trial_info* info) {
    int i = t->n_run;
    ytr_trial* h;
    if (i < 0 || i >= YTR_MAX_TRIALS) return YTR_ERR_FULL;   /* callers checked */
    h = &t->history[i];
    if (t->break_pending & YTR__BREAK_FLAG) {
        flags |= YTR_FLAG_AFTER_BREAK;
        t->break_pending &= ~YTR__BREAK_FLAG;
    }
    h->condition = (int16_t)cond;
    h->track = (int8_t)track;
    h->flags = (uint8_t)flags;
    h->rep = (int16_t)rep;
    h->block = (int16_t)block;
    h->outcome = YTR_INVALID;
    t->n_run = i + 1;
    t->current = i;
    if (t->n_jit > 0) ytr__jit_draw(t, i, cond);
    ytr__info(t, i, info);
    return i;
}

static int ytr__pick_track(ytr_trials* t, uint32_t live) {
    const ytr_desc* d = &t->desc;
    int i, k, n = 0, last = -1;
    double total = 0.0, u, acc = 0.0;
    if (d->interleave == YTR_INTERLEAVE_ROUND_ROBIN) {
        for (k = 0; k < ytr__ntr(t); k++) {
            i = (t->rr_next + k) % ytr__ntr(t);
            if (live & ((uint32_t)1 << i)) {
                t->rr_next = (i + 1) % ytr__ntr(t);
                return i;
            }
        }
        return -1;
    }
    for (i = 0; i < d->n_tracks && i < YTR_MAX_TRACKS; i++) {
        if (!(live & ((uint32_t)1 << i))) continue;
        n++;
        total += d->tracks[i].weight;
        last = i;
    }
    if (n <= 1) return last;
    u = d->rng(d->rng_ctx) * total;
    for (i = 0; i < d->n_tracks && i < YTR_MAX_TRACKS; i++) {
        if (!(live & ((uint32_t)1 << i))) continue;
        acc += d->tracks[i].weight;
        if (u < acc) return i;
    }
    return last;
}

/* Move slot `from` to slot `to` (< from), the slots between shifting up by
 * one, so the rest of the schedule keeps its order. */
static void ytr__move_forward(ytr_trials* t, int from, int to) {
    int16_t c, r;
    uint8_t f;
    size_t n;
    /* The size must be visibly nonnegative and inside the arrays, or gcc 13
     * reads a signed difference as a possible huge memmove. */
    if (to < 0 || from <= to || from >= YTR_MAX_TRIALS) return;
    n = (size_t)(unsigned)(from - to);
    c = t->schedule[from];
    r = t->schedule_rep[from];
    f = t->schedule_flags[from];
    memmove(t->schedule + to + 1, t->schedule + to, n * sizeof(t->schedule[0]));
    memmove(t->schedule_rep + to + 1, t->schedule_rep + to, n * sizeof(t->schedule_rep[0]));
    memmove(t->schedule_flags + to + 1, t->schedule_flags + to, n * sizeof(t->schedule_flags[0]));
    t->schedule[to] = c;
    t->schedule_rep[to] = r;
    t->schedule_flags[to] = f;
}

/* Move slots [from, from + len) to slot `to` (< from), the slots between
 * shifting up by len, so the rest of the schedule keeps its order. */
static void ytr__move_forward_n(ytr_trials* t, int from, int len, int to) {
    int16_t* tc = t->work[0];
    int16_t* tr = t->work[1];
    size_t n;
    int j;
    uint8_t rq;
    if (to < 0 || from <= to || len < 1 || from + len > YTR_MAX_TRIALS) return;
    if (len == 1) {
        ytr__move_forward(t, from, to);
        return;
    }
    for (j = 0; j < len; j++) {
        tc[j] = t->schedule[from + j];
        tr[j] = t->schedule_rep[from + j];
    }
    /* A unit's slots carry CONT after the first and one REQUEUED bit for
     * all (a re-queue copies whole units); nothing else, as the search
     * stays inside the block and units never hold a lead-in. */
    rq = (uint8_t)(t->schedule_flags[from] & YTR_FLAG_REQUEUED);
    n = (size_t)(unsigned)(from - to);
    memmove(t->schedule + to + len, t->schedule + to, n * sizeof(t->schedule[0]));
    memmove(t->schedule_rep + to + len, t->schedule_rep + to, n * sizeof(t->schedule_rep[0]));
    memmove(t->schedule_flags + to + len, t->schedule_flags + to, n * sizeof(t->schedule_flags[0]));
    for (j = 0; j < len; j++) {
        t->schedule_flags[to + j] = (uint8_t)(rq | (j > 0 ? YTR__SLOT_CONT : 0));
        t->schedule[to + j] = tc[j];
        t->schedule_rep[to + j] = tr[j];
    }
}

/* The end of the BLOCKED group block that slot pos is in (the next slot
 * flagged as a block start), or n_scheduled. */
static int ytr__block_end(const ytr_trials* t, int pos) {
    int k, n = ytr__nsch(t);
    if (t->desc.groups.mode != YTR_GROUPS_BLOCKED) return n;
    for (k = pos + 1; k < n; k++)
        if (t->schedule_flags[k] & YTR__SLOT_BLOCK) return k;
    return n;
}

/* next() for a session with units, groups or a lead-in. The v0.1 rules,
 * plus: inside a unit, the next trial of the unit runs (no track trial, no
 * draw, no forward move, no block start); a block starts at a group's
 * first slot (BLOCKED) or at the first unit start at or after each
 * block_size boundary; a forward move moves a whole unit. */
static int ytr__next2(ytr_trials* t, ytr_trial_info* info) {
    const ytr_desc* d = &t->desc;
    uint32_t live = 0, bit;
    int i, q, bs, block, flags, cond, track, rep, seg, pos, len = 1, j, hi;
    bool has_sched, starts_block, seg_new, use_track, viol, in_unit, fits = false;

    if (t->schedule_pos < d->n_practice) {
        if (t->n_run >= YTR_MAX_TRIALS) return YTR_ERR_FULL;
        flags = YTR_FLAG_PRACTICE;
        if (t->schedule_pos == 0) flags |= YTR_FLAG_FIRST_IN_BLOCK;
        cond = t->schedule[t->schedule_pos++];
        return ytr__push(t, cond, -1, -1, -1, flags, info);
    }
    for (i = 0; i < d->n_tracks && i < YTR_MAX_TRACKS; i++) {
        bit = (uint32_t)1 << i;
        if (t->track_done & bit) continue;
        if (d->tracks[i].is_done(d->tracks[i].ctx)) t->track_done |= bit;
        else live |= bit;
    }
    has_sched = t->schedule_pos < t->n_scheduled;
    if (!has_sched && live == 0) return YTR_DONE;
    if (t->n_run >= YTR_MAX_TRIALS) return YTR_ERR_FULL;

    pos = ytr__clamp(t->schedule_pos, YTR_MAX_TRIALS - 1);
    in_unit = has_sched && (t->schedule_flags[pos] & YTR__SLOT_CONT);
    bs = d->block_size;
    if (in_unit) starts_block = false;
    else if (d->groups.mode == YTR_GROUPS_BLOCKED)
        starts_block = t->n_main == 0 || (has_sched && (t->schedule_flags[pos] & YTR__SLOT_BLOCK));
    else
        starts_block = t->n_main == 0 || (bs > 0 && t->n_main >= t->blk_next);
    block = starts_block ? t->blk + 1 : t->blk;
    if (block < 0) block = 0;

    if (starts_block && t->n_main > 0 && d->n_warmup > 0) {
        if (t->warm_block != block) {
            t->warm_block = block;
            t->warm_done = 0;
        }
        if (t->warm_done < d->n_warmup) {
            flags = YTR_FLAG_WARMUP;
            if (t->warm_done == 0) flags |= YTR_FLAG_FIRST_IN_BLOCK | YTR_FLAG_AFTER_BREAK;
            cond = ytr__pick_easy(t, t->warm_done);
            t->warm_done++;
            return ytr__push(t, cond, -1, -1, block, flags, info);
        }
    }

    flags = 0;
    if (starts_block && !(t->n_main > 0 && d->n_warmup > 0)) {
        flags |= YTR_FLAG_FIRST_IN_BLOCK;
        if (t->n_main > 0) flags |= YTR_FLAG_AFTER_BREAK;
    }
    seg_new = !d->constraints_span_blocks &&
              (starts_block || (t->break_pending & YTR__BREAK_SEG) != 0);
    if (d->constraints_span_blocks) seg = 0;
    else seg = seg_new ? t->n_main : t->seg_start;

    viol = false;
    if (has_sched && d->n_constraints > 0) {
        t->main_seq[t->n_main] = t->schedule[pos];
        viol = ytr__violation(t, t->main_seq, seg, t->n_main) >= 0;
    }

    if (in_unit) use_track = false;
    else if (live != 0 && has_sched) {
        if (viol || d->track_rate >= 1.0) use_track = true;
        else use_track = d->rng(d->rng_ctx) < d->track_rate;
    } else {
        use_track = live != 0;
    }

    if (use_track) {
        track = ytr__pick_track(t, live);
        cond = -1;
        rep = -1;
    } else {
        if (starts_block) t->schedule_flags[pos] &= (uint8_t)~YTR__SLOT_BLOCK;
        if (viol && !in_unit) {
            /* The first later unit all of whose trials fit moves forward. */
            hi = ytr__block_end(t, pos);
            for (q = pos + ytr__unit_len(t->schedule_flags, pos, hi); q < hi; q += len) {
                len = ytr__unit_len(t->schedule_flags, q, hi);
                fits = true;
                for (j = 0; j < len && fits; j++) {
                    if (t->n_main + j >= YTR_MAX_TRIALS) { fits = false; break; }
                    t->main_seq[t->n_main + j] = t->schedule[q + j];
                    if (ytr__violation(t, t->main_seq, seg, t->n_main + j) >= 0) fits = false;
                }
                if (fits) break;
            }
            if (q < hi && fits) ytr__move_forward_n(t, q, len, pos);
            else flags |= YTR_FLAG_VIOLATION;
        } else if (viol) {
            flags |= YTR_FLAG_VIOLATION;
        }
        track = -1;
        cond = t->schedule[pos];
        rep = t->schedule_rep[pos];
        if (t->schedule_flags[pos] & YTR_FLAG_REQUEUED) flags |= YTR_FLAG_REQUEUED;
        if (t->schedule_flags[pos] & YTR__SLOT_LEADIN) flags |= YTR_FLAG_LEADIN;
        t->schedule_pos++;
    }

    t->main_seq[t->n_main] = (int16_t)cond;
    if (seg_new) t->seg_start = t->n_main;
    if (starts_block) {
        t->blk = block;
        if (bs > 0) t->blk_next = (t->n_main / bs + 1) * bs;
    }
    t->n_main++;
    t->break_pending &= ~YTR__BREAK_SEG;
    return ytr__push(t, cond, track, rep, block, flags, info);
}


YTR_API int ytr_next(ytr_trials* t, ytr_trial_info* info) {
    const ytr_desc* d;
    uint32_t live = 0, bit;
    int i, q, bs, block, flags, cond, track, rep, seg;
    bool has_sched, starts_block, seg_new, use_track, viol;

    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (t->current >= 0 && t->current < YTR_MAX_TRIALS) {
        ytr__info(t, t->current, info);
        return t->current;
    }
    if (ytr__run2(t)) return ytr__next2(t, info);
    d = &t->desc;

    if (t->schedule_pos < d->n_practice) {
        if (t->n_run >= YTR_MAX_TRIALS) return YTR_ERR_FULL;
        flags = YTR_FLAG_PRACTICE;
        if (t->schedule_pos == 0) flags |= YTR_FLAG_FIRST_IN_BLOCK;
        cond = t->schedule[t->schedule_pos++];
        return ytr__push(t, cond, -1, -1, -1, flags, info);
    }

    for (i = 0; i < d->n_tracks && i < YTR_MAX_TRACKS; i++) {
        bit = (uint32_t)1 << i;
        if (t->track_done & bit) continue;
        if (d->tracks[i].is_done(d->tracks[i].ctx)) t->track_done |= bit;
        else live |= bit;
    }
    has_sched = t->schedule_pos < t->n_scheduled;
    if (!has_sched && live == 0) return YTR_DONE;
    if (t->n_run >= YTR_MAX_TRIALS) return YTR_ERR_FULL;

    bs = d->block_size;
    block = bs > 0 ? t->n_main / bs : 0;
    starts_block = t->n_main == 0 || (bs > 0 && t->n_main % bs == 0);

    /* n_warmup > 0 implies bs > 0, which open() checked. */
    if (starts_block && t->n_main > 0 && d->n_warmup > 0) {
        if (t->warm_block != block) {
            t->warm_block = block;
            t->warm_done = 0;
        }
        if (t->warm_done < d->n_warmup) {
            flags = YTR_FLAG_WARMUP;
            if (t->warm_done == 0) flags |= YTR_FLAG_FIRST_IN_BLOCK | YTR_FLAG_AFTER_BREAK;
            cond = ytr__pick_easy(t, t->warm_done);
            t->warm_done++;
            return ytr__push(t, cond, -1, -1, block, flags, info);
        }
    }

    flags = 0;
    if (starts_block && !(t->n_main > 0 && d->n_warmup > 0)) {
        flags |= YTR_FLAG_FIRST_IN_BLOCK;
        if (t->n_main > 0) flags |= YTR_FLAG_AFTER_BREAK;
    }
    seg_new = !d->constraints_span_blocks &&
              (starts_block || (t->break_pending & YTR__BREAK_SEG) != 0);
    if (d->constraints_span_blocks) seg = 0;
    else seg = seg_new ? t->n_main : t->seg_start;

    viol = false;
    if (has_sched && d->n_constraints > 0) {
        t->main_seq[t->n_main] = t->schedule[t->schedule_pos];
        viol = ytr__violation(t, t->main_seq, seg, t->n_main) >= 0;
    }

    if (live != 0 && has_sched) {
        if (viol || d->track_rate >= 1.0) use_track = true;
        else use_track = d->rng(d->rng_ctx) < d->track_rate;
    } else {
        use_track = live != 0;
    }

    if (use_track) {
        track = ytr__pick_track(t, live);
        cond = -1;
        rep = -1;
    } else {
        if (viol) {
            for (q = t->schedule_pos + 1; q < ytr__nsch(t); q++) {
                t->main_seq[t->n_main] = t->schedule[q];
                if (ytr__violation(t, t->main_seq, seg, t->n_main) < 0) break;
            }
            if (q < ytr__nsch(t)) ytr__move_forward(t, q, t->schedule_pos);
            else flags |= YTR_FLAG_VIOLATION;
        }
        track = -1;
        cond = t->schedule[t->schedule_pos];
        rep = t->schedule_rep[t->schedule_pos];
        if (t->schedule_flags[t->schedule_pos] & YTR_FLAG_REQUEUED) flags |= YTR_FLAG_REQUEUED;
        t->schedule_pos++;
    }

    t->main_seq[t->n_main] = (int16_t)cond;
    if (seg_new) t->seg_start = t->n_main;
    t->n_main++;
    t->break_pending &= ~YTR__BREAK_SEG;
    return ytr__push(t, cond, track, rep, block, flags, info);
}

/* The caller's slot for trial i, or NULL when there are no records. */
static unsigned char* ytr__slot(const ytr_trials* t, int i) {
    if (t->desc.record_size == 0 || !t->desc.records) return NULL;
    return (unsigned char*)t->desc.records + (size_t)i * t->desc.record_size;
}

static void ytr__finish(ytr_trials* t, int outcome) {
    ytr_trial* h = &t->history[t->current];
    h->outcome = outcome;
    h->flags |= YTR_FLAG_DONE;
    if (h->condition >= 0 && h->condition < YTR_MAX_CONDITIONS && outcome >= 0 &&
        !(h->flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP | YTR_FLAG_LEADIN))) {
        t->tally_valid[h->condition]++;
        if (outcome == 1) t->tally_pos[h->condition]++;
    }
    t->n_done++;
    t->current = -1;
}

YTR_API int ytr_update(ytr_trials* t, int outcome, const void* rec) {
    unsigned char* slot;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (t->current < 0 || t->current >= YTR_MAX_TRIALS) return YTR_ERR_ORDER;
    if (outcome < YTR_INVALID) return YTR_ERR_ARG;
    slot = ytr__slot(t, t->current);
    if (slot) {
        if (rec) memcpy(slot, rec, t->desc.record_size);
        else memset(slot, 0, t->desc.record_size);
    }
    ytr__finish(t, outcome);
    return 0;
}

YTR_API int ytr_mark_break(ytr_trials* t) {
    ytr_trial* h;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (t->current < 0 || t->current >= YTR_MAX_TRIALS) {
        t->break_pending |= YTR__BREAK_FLAG | YTR__BREAK_SEG;
        return 0;
    }
    h = &t->history[t->current];
    h->flags |= YTR_FLAG_AFTER_BREAK;
    if (h->flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP)) t->break_pending |= YTR__BREAK_SEG;
    else t->seg_start = t->n_main - 1;
    return 0;
}

/* requeue() for a session with units, groups or a lead-in: a copy of the
 * whole unit, inserted at a unit boundary (inside the BLOCKED group's
 * block), at or after requeue_gap and the unit's end. Draws: with a gap,
 * one index draw among those boundaries (as v0.1 draws among slots). */
static int ytr__requeue2(ytr_trials* t, ytr_trial* h) {
    const ytr_desc* d = &t->desc;
    int np = ytr__clamp(d->n_practice, YTR_MAX_TRIALS), cur, us, ue, len, left, hi, p, lo, m, k, j;
    size_t n;
    if (h->flags & YTR_FLAG_LEADIN) return YTR_ERR_ARG;
    cur = t->schedule_pos - 1;
    if (cur < np || cur >= ytr__nsch(t)) return YTR_ERR_ARG;
    {
        unsigned char* slot = ytr__slot(t, t->current);
        if (slot) memset(slot, 0, d->record_size);
    }
    us = cur;
    while (us > np && (t->schedule_flags[us] & YTR__SLOT_CONT)) us--;
    ue = cur + 1;
    while (ue < t->n_scheduled && (t->schedule_flags[ue] & YTR__SLOT_CONT)) ue++;
    len = ue - us;
    left = t->n_scheduled - t->schedule_pos;
    if (t->n_scheduled + len > YTR_MAX_TRIALS || t->n_run + left + len > YTR_MAX_TRIALS) {
        ytr__finish(t, YTR_INVALID);
        return YTR_ERR_FULL;
    }
    hi = ytr__block_end(t, cur);
    p = hi;
    if (d->requeue_gap > 0) {
        lo = t->schedule_pos + d->requeue_gap;
        if (lo < ue) lo = ue;
        if (lo < hi) {
            for (m = 0, k = lo; k <= hi; k++)
                if (k == hi || !(t->schedule_flags[k] & YTR__SLOT_CONT)) m++;
            j = ytr__draw(t, m);
            for (k = lo; k <= hi; k++)
                if (k == hi || !(t->schedule_flags[k] & YTR__SLOT_CONT)) {
                    if (j-- == 0) break;
                }
            p = k > hi ? hi : k;
        } else {
            (void)ytr__draw(t, 1);
        }
    }
    if (p < ue || p > t->n_scheduled || t->n_scheduled + len > YTR_MAX_TRIALS) return YTR_ERR_FULL;
    n = (size_t)(unsigned)(t->n_scheduled - p);
    memmove(t->schedule + p + len, t->schedule + p, n * sizeof(t->schedule[0]));
    memmove(t->schedule_rep + p + len, t->schedule_rep + p, n * sizeof(t->schedule_rep[0]));
    memmove(t->schedule_flags + p + len, t->schedule_flags + p, n * sizeof(t->schedule_flags[0]));
    for (j = 0; j < len; j++) {
        t->schedule[p + j] = t->schedule[us + j];
        t->schedule_rep[p + j] = t->schedule_rep[us + j];
        t->schedule_flags[p + j] = (uint8_t)(YTR_FLAG_REQUEUED | (j > 0 ? YTR__SLOT_CONT : 0));
    }
    t->n_scheduled += len;
    ytr__finish(t, YTR_REQUEUE);
    return 0;
}

YTR_API int ytr_requeue(ytr_trials* t) {
    const ytr_desc* d;
    ytr_trial* h;
    unsigned char* slot;
    int p, lo, left;
    size_t n;

    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (t->current < 0 || t->current >= YTR_MAX_TRIALS) return YTR_ERR_ORDER;
    d = &t->desc;
    h = &t->history[t->current];
    if (h->track >= 0 || (h->flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP)))
        return YTR_ERR_ARG;
    if (ytr__run2(t)) return ytr__requeue2(t, h);

    slot = ytr__slot(t, t->current);
    if (slot) memset(slot, 0, d->record_size);

    left = t->n_scheduled - t->schedule_pos;
    if (t->n_scheduled >= YTR_MAX_TRIALS || t->n_run + left + 1 > YTR_MAX_TRIALS) {
        ytr__finish(t, YTR_INVALID);
        return YTR_ERR_FULL;
    }

    p = t->n_scheduled;
    if (d->requeue_gap > 0) {
        lo = t->schedule_pos + d->requeue_gap;
        if (lo < t->n_scheduled) p = lo + ytr__draw(t, t->n_scheduled - lo + 1);
        else (void)ytr__draw(t, 1);
    }
    /* n_scheduled < YTR_MAX_TRIALS was checked above; restated so the size
     * is visibly inside the arrays (see ytr__move_forward). */
    if (p < 0 || p > t->n_scheduled || t->n_scheduled >= YTR_MAX_TRIALS) return YTR_ERR_FULL;
    n = (size_t)(unsigned)(t->n_scheduled - p);
    memmove(t->schedule + p + 1, t->schedule + p, n * sizeof(t->schedule[0]));
    memmove(t->schedule_rep + p + 1, t->schedule_rep + p, n * sizeof(t->schedule_rep[0]));
    memmove(t->schedule_flags + p + 1, t->schedule_flags + p, n * sizeof(t->schedule_flags[0]));
    t->schedule[p] = h->condition;
    t->schedule_rep[p] = h->rep;
    t->schedule_flags[p] = YTR_FLAG_REQUEUED;
    t->n_scheduled++;

    ytr__finish(t, YTR_REQUEUE);
    return 0;
}

YTR_API bool ytr_done(const ytr_trials* t) {
    int i;
    if (!t || !t->open) return false;
    if (t->current >= 0 || t->schedule_pos < t->n_scheduled) return false;
    for (i = 0; i < ytr__ntr(t); i++) {
        if (t->track_done & ((uint32_t)1 << i)) continue;
        if (!t->desc.tracks[i].is_done(t->desc.tracks[i].ctx)) return false;
    }
    return true;
}

YTR_API int ytr_n_run(const ytr_trials* t) {
    return (t && t->open) ? t->n_run : 0;
}

YTR_API int ytr_n_done(const ytr_trials* t) {
    return (t && t->open) ? t->n_done : 0;
}

/* --- tallies ----------------------------------------------------------- */

YTR_API int ytr_n_valid(const ytr_trials* t, int condition) {
    if (!t || !t->open || condition < 0 || condition >= t->n_cond) return 0;
    return t->tally_valid[condition];
}

YTR_API int ytr_count(const ytr_trials* t, int condition, int outcome) {
    int i, n = 0;
    const ytr_trial* h;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (condition < 0 || condition >= t->n_cond) return YTR_ERR_ARG;
    if (outcome == 1) return t->tally_pos[condition];
    for (i = 0; i < ytr__nrun(t); i++) {
        h = &t->history[i];
        if (h->condition != condition || !(h->flags & YTR_FLAG_DONE)) continue;
        if (h->flags & (YTR_FLAG_PRACTICE | YTR_FLAG_WARMUP)) continue;
        if (h->outcome == outcome) n++;
    }
    return n;
}

YTR_API double ytr_proportion(const ytr_trials* t, int condition, int outcome) {
    int n, k;
    if (!t || !t->open || condition < 0 || condition >= t->n_cond || outcome < 0)
        return YTR__NAN;
    n = t->tally_valid[condition];
    if (n == 0) return YTR__NAN;
    k = ytr_count(t, condition, outcome);
    return (double)k / (double)n;
}

/* --- history ----------------------------------------------------------- */

YTR_API const ytr_trial* ytr_history(const ytr_trials* t, int* n) {
    if (!t || !t->open) {
        if (n) *n = 0;
        return NULL;
    }
    if (n) *n = t->n_run;
    return t->history;
}

YTR_API const void* ytr_record(const ytr_trials* t, int i) {
    if (!t || !t->open || i < 0 || i >= t->n_run) return NULL;
    return ytr__slot(t, i);
}

YTR_API ytr_jitter_value ytr_jitter(const ytr_trials* t, int i, int j) {
    if (!t || !t->open || i < 0 || i >= t->n_run || i >= YTR_MAX_TRIALS || j < 0 || j >= t->n_jit ||
        j >= YTR_MAX_JITTERS)
        return ytr__jit_bad();
    return ytr__jit_value(t->jit_raw[i][j], t->jit[j].rate_num, t->jit[j].rate_den);
}

YTR_API int ytr_jitter_index(const ytr_trials* t, const char* name) {
    int j;
    if (!t || !t->open || !name) return -1;
    for (j = 0; j < t->n_jit && j < YTR_MAX_JITTERS; j++)
        if (strcmp(t->jit[j].name, name) == 0) return j;
    return -1;
}

/* A CSV field, quoted when it would otherwise split or break the line. */
static void ytr__csv_name(ytr__str* s, const char* name, int f) {
    const char* p;
    bool quote = false;
    if (!name) {
        ytr__cat(s, "factor%d", f);
        return;
    }
    for (p = name; *p; p++)
        if (*p == ',' || *p == '"' || *p == '\n' || *p == '\r') quote = true;
    if (!quote) {
        ytr__cat(s, "%s", name);
        return;
    }
    ytr__cat(s, "\"");
    for (p = name; *p; p++) {
        if (*p == '"') ytr__cat(s, "\"\"");
        else ytr__cat(s, "%c", *p);
    }
    ytr__cat(s, "\"");
}

YTR_API int ytr_format_header(const ytr_trials* t, char* buf, size_t cap) {
    ytr__str s;
    int f;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    ytr__str_init(&s, buf, cap);
    ytr__cat(&s, "index,block,rep,condition,track,practice,warmup,requeued,after_break,");
    if (t->has_leadin) ytr__cat(&s, "leadin,");
    ytr__cat(&s, "outcome");
    for (f = 0; f < ytr__nfe(t); f++) {
        ytr__cat(&s, ",");
        if (t->desc.table) ytr__cat(&s, "%s", ytb_col_name(t->desc.table, f));
        else ytr__csv_name(&s, t->desc.factors[f].name, f);
    }
    for (f = 0; f < t->n_jit && f < YTR_MAX_JITTERS; f++) ytr__cat(&s, ",%s", t->jit[f].name);
    ytr__cat(&s, "\n");
    return ytr__str_done(&s);
}

YTR_API int ytr_format_row(const ytr_trials* t, int i, char* buf, size_t cap) {
    ytr__str s;
    const ytr_trial* h;
    int f;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (i < 0 || i >= t->n_run) return YTR_ERR_ARG;
    h = &t->history[i];
    ytr__str_init(&s, buf, cap);
    ytr__cat(&s, "%d,%d,%d,%d,%d,%d,%d,%d,%d,", i, (int)h->block, (int)h->rep,
               (int)h->condition, (int)h->track,
               (h->flags & YTR_FLAG_PRACTICE) ? 1 : 0,
               (h->flags & YTR_FLAG_WARMUP) ? 1 : 0,
               (h->flags & YTR_FLAG_REQUEUED) ? 1 : 0,
               (h->flags & YTR_FLAG_AFTER_BREAK) ? 1 : 0);
    if (t->has_leadin) ytr__cat(&s, "%d,", (h->flags & YTR_FLAG_LEADIN) ? 1 : 0);
    if (h->flags & YTR_FLAG_DONE) ytr__cat(&s, "%d", (int)h->outcome);
    for (f = 0; f < ytr__nfe(t); f++) {
        if (h->condition < 0) {
            ytr__cat(&s, ",");
        } else if (t->desc.table) {
            /* The cell's text as written, so the data file reads like the
             * conditions file. */
            ytr__cat(&s, ",");
            ytr__csv_name(&s, ytb_text(t->desc.table, h->condition, f), f);
        } else {
            ytr__cat(&s, ",%d", ytr__lv(t, h->condition, f));
        }
    }
    for (f = 0; f < t->n_jit && f < YTR_MAX_JITTERS; f++) {
        /* Seconds as the exact decimal of the nanoseconds used. */
        ytr_jitter_value v = ytr_jitter(t, i, f);
        ytr__cat(&s, ",%lld.%09lld", (long long)(v.ns / 1000000000), (long long)(v.ns % 1000000000));
    }
    ytr__cat(&s, "\n");
    return ytr__str_done(&s);
}

static void ytr__fr_factor(ytr__str* s, const ytr_trials* t, int f);

/* x in the fewest digits that read back as x (15 to 17), with '.' for
 * the decimal point whatever LC_NUMERIC says, so the text parses back. */
static void ytr__cat_num(ytr__str* s, double x) {
    char b[40];
    double y;
    int prec;
    size_t i, n = 0;
    for (prec = 15; prec <= 17; prec++) {
        snprintf(b, sizeof(b), "%.*g", prec, x);
        for (i = 0; b[i]; i++)
            if (!((b[i] >= '0' && b[i] <= '9') || b[i] == 'e' || b[i] == 'E' || b[i] == '-' || b[i] == '+'))
                b[i] = '.';
        n = i;
        if (ytb_parse_number(b, n, &y) == YTB_NUM_OK && y == x) break;
    }
    ytr__cat(s, "%s", b);
}

/* Jitter j as the arguments of its rules statement, `sep` between them. */
static void ytr__fr_jitter(ytr__str* s, const ytr_trials* t, int j, char sep) {
    static const char* const dn[] = { "uniform", "choice", "exponential" };
    const ytr__jit* q = &t->jit[j];
    double num[3];
    int k, np;
    ytr__cat(s, "%s%c%s", q->name, sep, dn[q->dist]);
    if (q->dist == (int)YTR_JITTER_CHOICE) {
        for (k = 0; k < q->n_values; k++) {
            ytr__cat(s, "%c", sep);
            ytr__cat_num(s, q->values[k]);
        }
    } else {
        num[0] = q->lo;
        num[1] = q->hi;
        num[2] = q->scale;
        np = q->dist == (int)YTR_JITTER_EXPONENTIAL ? 3 : 2;
        for (k = 0; k < np; k++) {
            ytr__cat(s, "%c", sep);
            if (q->col[k] >= 0) ytr__fr_factor(s, t, q->col[k]);
            else ytr__cat_num(s, num[k]);
        }
    }
    if (q->rate_num > 0) ytr__cat(s, "%crate=%d/%d", sep, q->rate_num, q->rate_den);
}

YTR_API int ytr_format_meta(const ytr_trials* t, char* buf, size_t cap) {
    static const char* const orders[] = { "sequential", "random", "full_random", "constrained",
                                          "list", "with_replacement" };
    static const char* const gmodes[] = { "none", "blocked", "alternate" };
    static const char* const gorders[] = { "sequential", "random", "latin", "balanced_latin", "list" };
    const ytr_desc* d;
    ytr__str s;
    int i;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    d = &t->desc;
    ytr__str_init(&s, buf, cap);
    ytr__cat(&s, "ysp_trials=%s conditions=%d factors=%d levels=", YTR_VERSION_STRING,
               t->n_cond, d->n_factors);
    for (i = 0; i < ytr__nf(t); i++)
        ytr__cat(&s, i ? "x%d" : "%d", d->factors[i].n_levels);
    if (t->has_cond_reps) {
        ytr__cat(&s, " cond_reps=");
        for (i = 0; i < ytr__nc(t); i++) ytr__cat(&s, i ? ",%d" : "%d", (int)t->cond_reps[i]);
    } else {
        ytr__cat(&s, " reps=%d", d->reps);
    }
    ytr__cat(&s, " order=%s constraints=", orders[d->order]);
    for (i = 0; i < ytr__nci(t); i++) {
        if (i) ytr__cat(&s, ";");
        ytr__describe(&s, &d->constraints[i]);
    }
    ytr__cat(&s, " max_swaps=%d swaps=%d span_blocks=%d tracks=%d interleave=%s weights=",
               d->max_swaps, t->swaps, d->constraints_span_blocks ? 1 : 0, d->n_tracks,
               d->interleave == YTR_INTERLEAVE_ROUND_ROBIN ? "round_robin" : "random");
    for (i = 0; i < ytr__ntr(t); i++) ytr__cat(&s, i ? ",%.15g" : "%.15g", d->tracks[i].weight);
    ytr__cat(&s, " track_rate=%.15g block_size=%d practice=%d warmup=%d warmup_conditions=",
               d->track_rate, d->block_size, d->n_practice, d->n_warmup);
    for (i = 0; i < ytr__nwl(t); i++)
        ytr__cat(&s, i ? ",%d" : "%d", (int)t->warm_list[i]);
    ytr__cat(&s, " requeue_gap=%d record_size=%lu scheduled=%d run=%d done=%d",
               d->requeue_gap, (unsigned long)d->record_size, t->n_scheduled, t->n_run,
               t->n_done);
    if (t->v2) {
        if (d->table)
            ytr__cat(&s, " table=%016llx rows=%d cols=%d", (unsigned long long)ytb_hash(d->table),
                       d->table->n_rows, d->table->n_cols);
        ytr__cat(&s, " subset=%d draws=%d list=%d groups=%s,%d,%s,%d aux=%016llx",
                   d->subset, d->draws, d->n_order_list, gmodes[d->groups.mode], d->groups.factor,
                   gorders[d->groups.order], d->groups.participant, (unsigned long long)t->aux_hash);
    }
    if (t->n_jit > 0) {
        ytr__cat(&s, " jitters=");
        for (i = 0; i < t->n_jit && i < YTR_MAX_JITTERS; i++) {
            if (i) ytr__cat(&s, ";");
            ytr__fr_jitter(&s, t, i, ':');
        }
    }
    ytr__cat(&s, "\n");
    return ytr__str_done(&s);
}

/* A factor's name in rules text: @row for the row itself, the column or
 * factor name, else f<index>. */
static void ytr__fr_factor(ytr__str* s, const ytr_trials* t, int f) {
    const char* nm = NULL;
    if (f == YTR_CONDITION) { ytr__cat(s, "@row"); return; }
    if (t->desc.table) nm = ytb_col_name(t->desc.table, f);
    else if (f >= 0 && f < YTR_MAX_FACTORS) nm = t->desc.factors[f].name;
    if (nm && *nm) {
        const char* p;
        for (p = nm; *p; p++)
            if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' ||
                  (p > nm && *p >= '0' && *p <= '9'))) break;
        if (!*p) { ytr__cat(s, "%s", nm); return; }
    }
    ytr__cat(s, "f%d", f);
}

/* A level in rules text: the table's text (quoted when it holds a blank,
 * a quote, '#', '=' or is empty), else the level number. */
static void ytr__fr_value(ytr__str* s, const ytr_trials* t, int f, int level) {
    const char* txt, *p;
    bool q = false;
    if (!t->desc.table || f == YTR_CONDITION) { ytr__cat(s, "%d", level); return; }
    txt = ytb_level_text(t->desc.table, f, level);
    if (!txt) { ytr__cat(s, "%d", level); return; }
    if (!*txt) q = true;
    for (p = txt; *p; p++)
        if (*p == ' ' || *p == '\t' || *p == '"' || *p == '#' || *p == '=' || *p == '\r' || *p == '\n') q = true;
    if (!q) { ytr__cat(s, "%s", txt); return; }
    ytr__cat(s, "\"");
    for (p = txt; *p; p++) ytr__cat(s, *p == '"' ? "\"\"" : "%c", *p);
    ytr__cat(s, "\"");
}

static void ytr__fr_sel(ytr__str* s, const ytr_trials* t, int f, int level) {
    ytr__fr_factor(s, t, f);
    if (level != YTR_ANY_LEVEL) {
        ytr__cat(s, "=");
        ytr__fr_value(s, t, f, level);
    }
}

YTR_API int ytr_format_rules(const ytr_trials* t, char* buf, size_t cap) {
    static const char* const orders[] = { "sequential", "random", "full_random", "constrained",
                                          "list", "with_replacement" };
    static const char* const gorders[] = { "sequential", "random", "latin", "balanced_latin", "list" };
    const ytr_desc* d;
    ytr__str s;
    int i, np;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    d = &t->desc;
    np = ytr__clamp(d->n_practice, YTR_MAX_TRIALS);
    ytr__str_init(&s, buf, cap);
    ytr__cat(&s, "# ysp_trials %s rules\n", YTR_VERSION_STRING);
    ytr__cat(&s, "order %s\n", orders[d->order]);
    if (d->order == YTR_ORDER_LIST) {
        ytr__cat(&s, "list");
        for (i = np; i < ytr__nsch(t); i++)
            if (!(t->schedule_flags[i] & YTR_FLAG_REQUEUED)) ytr__cat(&s, " %d", (int)t->schedule[i]);
        ytr__cat(&s, "\n");
    } else if (d->order == YTR_ORDER_WITH_REPLACEMENT) {
        ytr__cat(&s, "draws %d\n", d->draws);
        if (t->had_weights) ytr__cat(&s, "# weights were given in C; they are not repeated here\n");
    } else if (t->has_cond_reps) {
        ytr__cat(&s, "cond_reps");
        for (i = 0; i < ytr__nc(t); i++) ytr__cat(&s, " %d", (int)t->cond_reps[i]);
        ytr__cat(&s, "\n");
    } else if (t->n_cond > 0) {
        ytr__cat(&s, "reps %d\n", d->reps);
    }
    if (d->subset) ytr__cat(&s, "subset %d\n", d->subset);
    if (d->groups.mode != YTR_GROUPS_NONE) {
        ytr__cat(&s, "groups ");
        ytr__fr_factor(&s, t, d->groups.factor);
        ytr__cat(&s, " %s %s", d->groups.mode == YTR_GROUPS_BLOCKED ? "blocked" : "alternate",
                   gorders[d->groups.order]);
        if (d->groups.order == YTR_GROUP_ORDER_LIST) {
            /* The run order, read off the schedule: BLOCKED groups change
             * level once per block; ALTERNATE holds each group once in its
             * first cycle. */
            int prev = -1, k;
            for (i = np; i < ytr__nsch(t); i++) {
                int l = ytr__lv(t, t->schedule[i], d->groups.factor);
                if (d->groups.mode == YTR_GROUPS_ALTERNATE) {
                    for (k = np; k < i; k++)
                        if (ytr__lv(t, t->schedule[k], d->groups.factor) == l) break;
                    if (k < i) break;
                } else if (l == prev) {
                    continue;
                }
                ytr__cat(&s, " ");
                ytr__fr_value(&s, t, d->groups.factor, l);
                prev = l;
            }
        }
        ytr__cat(&s, "\n");
        if (d->groups.order == YTR_GROUP_ORDER_LATIN || d->groups.order == YTR_GROUP_ORDER_BALANCED_LATIN)
            ytr__cat(&s, "# participant %d\n", d->groups.participant);
    }
    if (d->block_size) ytr__cat(&s, "block_size %d\n", d->block_size);
    if (d->constraints_span_blocks) ytr__cat(&s, "span_blocks\n");
    if (d->n_practice) ytr__cat(&s, "practice %d\n", d->n_practice);
    if (d->n_warmup) ytr__cat(&s, "warmup %d\n", d->n_warmup);
    if (d->n_warmup_conditions) {
        ytr__cat(&s, "warmup_conditions");
        for (i = 0; i < ytr__nwl(t); i++) ytr__cat(&s, " %d", (int)t->warm_list[i]);
        ytr__cat(&s, "\n");
    }
    if (d->requeue_gap) ytr__cat(&s, "requeue_gap %d\n", d->requeue_gap);
    if (d->max_swaps != YTR__DEFAULT_SWAPS) ytr__cat(&s, "max_swaps %d\n", d->max_swaps);
    for (i = 0; i < ytr__nci(t); i++) {
        const ytr_constraint* c = &d->constraints[i];
        ytr__cat(&s, "%s ", ytr__rule_name(c->rule));
        switch (c->rule) {
        case YTR_RULE_MAX_RUN:
        case YTR_RULE_MIN_GAP:
            ytr__fr_sel(&s, t, c->factor, c->level);
            ytr__cat(&s, " %d", c->n);
            break;
        case YTR_RULE_MAX_IN_WINDOW:
            ytr__fr_sel(&s, t, c->factor, c->level);
            ytr__cat(&s, " %d %d", c->window, c->n);
            break;
        case YTR_RULE_FIRST_NOT:
            ytr__fr_sel(&s, t, c->factor, c->level);
            break;
        case YTR_RULE_NO_TRANSITION:
        case YTR_RULE_FOLLOWED_BY:
        case YTR_RULE_PRECEDED_BY:
            ytr__fr_factor(&s, t, c->factor);
            ytr__cat(&s, " ");
            ytr__fr_value(&s, t, c->factor, c->level);
            ytr__cat(&s, " ");
            ytr__fr_value(&s, t, c->factor, c->level2);
            break;
        case YTR_RULE_CHUNK:
            ytr__fr_factor(&s, t, c->factor);
            break;
        case YTR_RULE_BALANCE:
            ytr__fr_factor(&s, t, c->factor);
            if (c->n & YTR_BALANCE_NO_REPEAT) ytr__cat(&s, " no_repeat");
            if (c->n & YTR_BALANCE_NO_LEADIN) ytr__cat(&s, " no_leadin");
            break;
        default:
            break;
        }
        ytr__cat(&s, "\n");
    }
    for (i = 0; i < t->n_jit && i < YTR_MAX_JITTERS; i++) {
        ytr__cat(&s, "jitter ");
        ytr__fr_jitter(&s, t, i, ' ');
        ytr__cat(&s, "\n");
    }
    return ytr__str_done(&s);
}

/* --- rules text (GRAMMAR) ------------------------------------------------ */

#define YTR__RL_LINE 4096
#define YTR__RL_ARGS 64
#define YTR__RL_LINES 100000

/* One argument: `key` or `key=value`, either part possibly quoted. A value
 * with no key is a bare quoted token. */
typedef struct ytr__arg {
    const char* k;
    size_t      kn;
    bool        kq;      /* key quoted, may hold "" pairs */
    const char* v;       /* NULL: no '='                  */
    size_t      vn;
    bool        vq;
    int         col;     /* 1-based column in the line    */
} ytr__arg;

typedef struct ytr__rl {
    ytr_desc*             d;
    const ytr_rules_desc* r;
    char*                   err;
    size_t                  cap;
    int                     line;
    int                     n_cond;
    unsigned char*          arena;
    size_t                  used, size;
    int                     first[32];      /* line of each once-only statement */
    int*                    cond_reps;      /* weight / cond_reps / where        */
    unsigned char*          mask;           /* where: rows kept                  */
    int*                    list;
    int                     n_list;
    bool                    failed;
} ytr__rl;

static int ytr__rl_fail(ytr__rl* rl, int col, const char* fmt, ...) {
    va_list ap;
    int n = 0;
    if (rl->failed) return rl->line;
    rl->failed = true;
    if (rl->err && rl->cap > 0) {
        n = col > 0 ? snprintf(rl->err, rl->cap, "ysp_trials: rules line %d, col %d: ", rl->line, col)
                    : snprintf(rl->err, rl->cap, "ysp_trials: rules line %d: ", rl->line);
        if (n < 0) n = 0;
        if ((size_t)n < rl->cap) {
            va_start(ap, fmt);
            vsnprintf(rl->err + n, rl->cap - (size_t)n, fmt, ap);
            va_end(ap);
        }
    }
    return rl->line > 0 ? rl->line : 1;
}

static void* ytr__rl_alloc(ytr__rl* rl, size_t bytes) {
    size_t at = (rl->used + 7u) & ~(size_t)7u;
    if (!rl->arena || at + bytes > rl->size || at + bytes < at) {
        ytr__rl_fail(rl, 0, "the rules arena has %lu bytes and needs %lu", (unsigned long)rl->size,
                       (unsigned long)(at + bytes));
        return NULL;
    }
    rl->used = at + bytes;
    return rl->arena + at;
}

/* Token text equals a C string (quoted tokens compare with "" as "). */
static bool ytr__rl_eq(const char* p, size_t n, bool q, const char* s) {
    size_t i = 0, k = 0;
    if (!s) return false;
    while (i < n) {
        if (s[k] == '\0' || p[i] != s[k]) return false;
        i += (q && p[i] == '"') ? 2 : 1;
        k++;
    }
    return s[k] == '\0';
}

static bool ytr__rl_word(const ytr__arg* a, const char* w) {
    return !a->v && !a->kq && a->kn == strlen(w) && memcmp(a->k, w, a->kn) == 0;
}

/* A count: decimal digits, at most 9. -1 when not one. */
static int ytr__rl_count(const char* p, size_t n, bool q) {
    size_t i;
    int v = 0;
    if (q || n == 0 || n > 9) return -1;
    for (i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '9') return -1;
        v = v * 10 + (p[i] - '0');
    }
    return v;
}

static int ytr__rl_nfac(const ytr__rl* rl) {
    return rl->r->table ? rl->r->table->n_cols : rl->d->n_factors;
}

/* A factor by name: a table column, a factor name, f<i> for an unnamed
 * factor, or @row. -2 when unknown (with the message). */
static int ytr__rl_factor(ytr__rl* rl, const ytr__arg* a, const char* p, size_t n, bool q) {
    int f, nf = ytr__rl_nfac(rl);
    char names[160];
    size_t k = 0;
    if (!q && n == 4 && memcmp(p, "@row", 4) == 0) return YTR_CONDITION;
    for (f = 0; f < nf; f++) {
        const char* nm = rl->r->table ? ytb_col_name(rl->r->table, f)
                                      : (f < YTR_MAX_FACTORS ? rl->d->factors[f].name : NULL);
        if (nm && ytr__rl_eq(p, n, q, nm)) return f;
        if (!nm && !q && n >= 2 && p[0] == 'f' && ytr__rl_count(p + 1, n - 1, false) == f) return f;
    }
    names[0] = '\0';
    for (f = 0; f < nf && k + 24 < sizeof(names); f++) {
        const char* nm = rl->r->table ? ytb_col_name(rl->r->table, f)
                                      : (f < YTR_MAX_FACTORS ? rl->d->factors[f].name : NULL);
        int w = nm ? snprintf(names + k, sizeof(names) - k, "%s%.20s", f ? ", " : "", nm)
                   : snprintf(names + k, sizeof(names) - k, "%sf%d", f ? ", " : "", f);
        if (w > 0) k += (size_t)w;
    }
    if (f < nf && k + 6 < sizeof(names)) memcpy(names + k, ", ...", 6);
    ytr__rl_fail(rl, a->col, "unknown column '%.*s' (%s%s@row)", (int)(n > 40 ? 40 : n), p, names,
                   nf ? ", " : "");
    return -2;
}

/* A level of factor f from a value token; -2 when unknown (with message). */
static int ytr__rl_level(ytr__rl* rl, const ytr__arg* a, int f, const char* p, size_t n, bool q) {
    const ytb_table* tb = rl->r->table;
    int nl, l, v;
    if (!q && n == 12 && memcmp(p, "@participant", 12) == 0) {
        nl = (f == YTR_CONDITION) ? rl->n_cond : (tb ? ytb_n_levels(tb, f) : rl->d->factors[f].n_levels);
        if (nl < 1) { ytr__rl_fail(rl, a->col, "@participant: the column has no levels"); return -2; }
        return rl->r->participant % nl;
    }
    if (!tb || f == YTR_CONDITION) {
        nl = (f == YTR_CONDITION) ? rl->n_cond : rl->d->factors[f].n_levels;
        v = ytr__rl_count(p, n, q);
        if (v < 0 || v >= nl) {
            ytr__rl_fail(rl, a->col, "'%.*s' is not a level number below %d", (int)(n > 40 ? 40 : n), p, nl);
            return -2;
        }
        return v;
    }
    nl = ytb_n_levels(tb, f);
    if (ytb_col_type(tb, f) != YTB_STRING && !q && n > 0) {
        double want, got;
        char buf[64];
        if (n < sizeof(buf)) {
            memcpy(buf, p, n);
            buf[n] = '\0';
            if (ytb_parse_number(buf, n, &want) == YTB_NUM_OK)
                for (l = 0; l < nl; l++) {
                    got = ytb_level_num(tb, f, l);
                    if (got == want) return l;
                }
        }
    } else {
        for (l = 0; l < nl; l++)
            if (ytr__rl_eq(p, n, q, ytb_level_text(tb, f, l))) return l;
    }
    {
        char lv[120];
        size_t k = 0;
        for (l = 0; l < nl && l < 8 && k + 24 < sizeof(lv); l++) {
            int w = snprintf(lv + k, sizeof(lv) - k, "%s%.16s", l ? ", " : "", ytb_level_text(tb, f, l));
            if (w > 0) k += (size_t)w;
        }
        if (k == 0) lv[0] = '\0';
        ytr__rl_fail(rl, a->col, "column '%s' has no level '%.*s' (levels: %s%s)", ytb_col_name(tb, f),
                       (int)(n > 40 ? 40 : n), p, lv, nl > 8 ? ", ..." : "");
    }
    return -2;
}

/* A selector: COL or COL=VALUE. */
static bool ytr__rl_sel(ytr__rl* rl, const ytr__arg* a, int* f, int* level) {
    *f = ytr__rl_factor(rl, a, a->k, a->kn, a->kq);
    if (*f == -2) return false;
    if (!a->v) { *level = YTR_ANY_LEVEL; return true; }
    *level = ytr__rl_level(rl, a, *f, a->v, a->vn, a->vq);
    return *level != -2;
}

static bool ytr__rl_cnt(ytr__rl* rl, const ytr__arg* a, int lo, int* out) {
    int v = a->v ? -1 : ytr__rl_count(a->k, a->kn, a->kq);
    if (v < lo) {
        ytr__rl_fail(rl, a->col, "'%.*s' is not a count of at least %d", (int)(a->kn > 40 ? 40 : a->kn), a->k, lo);
        return false;
    }
    *out = v;
    return true;
}

static bool ytr__rl_add(ytr__rl* rl, ytr_constraint c) {
    if (rl->d->n_constraints >= YTR_MAX_CONSTRAINTS || rl->d->n_constraints < 0) {
        ytr__rl_fail(rl, 0, "more than %d constraints", YTR_MAX_CONSTRAINTS);
        return false;
    }
    rl->d->constraints[rl->d->n_constraints++] = c;
    return true;
}

/* A statement given once only: index into first[]. */
static bool ytr__rl_once(ytr__rl* rl, int which, const char* verb) {
    if (rl->first[which]) {
        ytr__rl_fail(rl, 0, "%s is given twice (first on line %d)", verb, rl->first[which]);
        return false;
    }
    rl->first[which] = rl->line;
    return true;
}

static int ytr__rl_lev_dist(const char* a, size_t an, const char* b) {
    /* Levenshtein distance, for "did you mean". */
    int row[32], i, j, bn = (int)strlen(b), prev, cur;
    if (an > 30 || bn > 30) return 99;
    for (j = 0; j <= bn; j++) row[j] = j;
    for (i = 1; i <= (int)an; i++) {
        prev = row[0];
        row[0] = i;
        for (j = 1; j <= bn; j++) {
            cur = row[j];
            row[j] = a[i - 1] == b[j - 1] ? prev
                   : 1 + (prev < row[j] ? (prev < row[j - 1] ? prev : row[j - 1])
                                        : (row[j] < row[j - 1] ? row[j] : row[j - 1]));
            prev = cur;
        }
    }
    return row[bn];
}

static const char* const ytr__rl_verbs[] = {
    "order", "reps", "cond_reps", "weight", "where", "subset", "draws", "list", "groups",
    "block_size", "practice", "warmup", "warmup_conditions", "requeue_gap", "max_swaps", "span_blocks",
    "max_run", "max_in_window", "min_gap", "no_transition", "first_not", "followed_by", "preceded_by",
    "chunk", "balance", "constrain", "shuffle", "jitter"
};

/* Spellings from other tools that mean a statement here. */
static const char* ytr__rl_alias(const char* p, size_t n) {
    static const char* const pairs[][2] = {
        { "maxrep", "max_run" }, { "max_rep", "max_run" }, { "maxrun", "max_run" },
        { "mindist", "min_gap" }, { "min_dist", "min_gap" }, { "mingap", "min_gap" },
        { "maxinwindow", "max_in_window" }, { "window", "max_in_window" },
        { "notransition", "no_transition" }, { "firstnot", "first_not" },
        { "follows", "followed_by" }, { "followedby", "followed_by" },
        { "precededby", "preceded_by" }, { "precedes", "preceded_by" },
        { "repetitions", "reps" }, { "nreps", "reps" }, { "size", "draws" },
        { "with_replacement", "order with_replacement" }, { "without_replacement", "subset" },
        { "randomize_order", "order random" }, { "shufflenorepeats", "max_run @row 1" },
        { "counterbalance", "groups ... latin" }, { "latin", "order latin" },
        { "iti", "jitter iti ..." }, { "foreperiod", "jitter foreperiod ..." }, { "isi", "jitter isi ..." },
        { "soa", "jitter soa ..." }
    };
    size_t i;
    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); i++)
        if (strlen(pairs[i][0]) == n && memcmp(pairs[i][0], p, n) == 0) return pairs[i][1];
    return NULL;
}

/* Split one line into args. Returns the count, or -1 (message set). */
static int ytr__rl_split(ytr__rl* rl, const char* p, size_t n, ytr__arg* out) {
    size_t i = 0;
    int na = 0;
    while (i < n) {
        ytr__arg* a;
        const char** tp;
        size_t* tn;
        bool* tq;
        int part;
        while (i < n && (p[i] == ' ' || p[i] == '\t')) i++;
        if (i >= n || p[i] == '#') break;
        if (na >= YTR__RL_ARGS) {
            ytr__rl_fail(rl, (int)i + 1, "more than %d arguments", YTR__RL_ARGS);
            return -1;
        }
        a = &out[na++];
        memset(a, 0, sizeof(*a));
        a->col = (int)i + 1;
        for (part = 0; part < 2; part++) {
            tp = part ? &a->v : &a->k;
            tn = part ? &a->vn : &a->kn;
            tq = part ? &a->vq : &a->kq;
            if (i < n && p[i] == '"') {
                size_t st = ++i;
                for (;;) {
                    if (i >= n) {
                        ytr__rl_fail(rl, a->col, "a quoted token is not closed");
                        return -1;
                    }
                    if (p[i] == '"') {
                        if (i + 1 < n && p[i + 1] == '"') { i += 2; continue; }
                        break;
                    }
                    i++;
                }
                *tp = p + st;
                *tn = i - st;
                *tq = true;
                i++;
            } else {
                size_t st = i;
                while (i < n && p[i] != ' ' && p[i] != '\t' && p[i] != '#' && p[i] != '=' && p[i] != '"') i++;
                *tp = p + st;
                *tn = i - st;
                if (i < n && p[i] == '"') {
                    ytr__rl_fail(rl, (int)i + 1, "a quote inside a token (quote the whole token)");
                    return -1;
                }
            }
            if (part == 0 && i < n && p[i] == '=') {
                i++;
                if (i >= n || p[i] == ' ' || p[i] == '\t' || p[i] == '#') {
                    ytr__rl_fail(rl, a->col, "'=' without a value");
                    return -1;
                }
                continue;
            }
            break;
        }
        if (i < n && p[i] != ' ' && p[i] != '\t' && p[i] != '#') {
            ytr__rl_fail(rl, (int)i + 1, "unexpected '%c'", p[i]);
            return -1;
        }
    }
    return na;
}

static bool ytr__rl_args(ytr__rl* rl, int na, int lo, int hi, const char* form) {
    if (na - 1 < lo || na - 1 > hi) {
        ytr__rl_fail(rl, 0, "expected: %s", form);
        return false;
    }
    return true;
}

enum {
    YTR__O_ORDER, YTR__O_REPS, YTR__O_CREPS, YTR__O_SUBSET, YTR__O_DRAWS, YTR__O_GROUPS,
    YTR__O_BLOCK, YTR__O_PRACTICE, YTR__O_WARMUP, YTR__O_WLIST, YTR__O_GAP, YTR__O_SWAPS,
    YTR__O_SPAN
};

static bool ytr__rl_order_name(ytr__rl* rl, const ytr__arg* a) {
    static const char* const names[] = { "sequential", "random", "full_random", "constrained", "list",
                                         "with_replacement" };
    int i;
    for (i = 0; i < 6; i++)
        if (ytr__rl_word(a, names[i])) { rl->d->order = (ytr_order)i; return true; }
    if (ytr__rl_word(a, "latin") || ytr__rl_word(a, "balanced_latin")) {
        bool bal = ytr__rl_word(a, "balanced_latin");
        int n = rl->n_cond, rows, j;
        int* lst;
        if (n < 1) { ytr__rl_fail(rl, a->col, "order %s needs the rows (a table or factors)", bal ? "balanced_latin" : "latin"); return false; }
        lst = (int*)ytr__rl_alloc(rl, sizeof(int) * (size_t)n);
        if (!lst) return false;
        rows = ytr__latin_rows(n, bal);
        for (j = 0; j < n; j++) lst[j] = ytr__latin_at(n, rl->r->participant % rows, bal, j);
        rl->list = lst;
        rl->n_list = n;
        rl->d->order = YTR_ORDER_LIST;
        return true;
    }
    ytr__rl_fail(rl, a->col, "unknown order '%.*s' (sequential, random, full_random, constrained, list, "
                   "with_replacement, latin, balanced_latin)", (int)(a->kn > 30 ? 30 : a->kn), a->k);
    return false;
}

/* The per-row repetition array, made on first use from reps (or 1). */
static int* ytr__rl_creps(ytr__rl* rl) {
    if (!rl->cond_reps) {
        int c;
        rl->cond_reps = (int*)ytr__rl_alloc(rl, sizeof(int) * (size_t)(rl->n_cond > 0 ? rl->n_cond : 1));
        if (!rl->cond_reps) return NULL;
        for (c = 0; c < rl->n_cond; c++) rl->cond_reps[c] = -1;   /* not set yet */
    }
    return rl->cond_reps;
}

static bool ytr__rl_stmt(ytr__rl* rl, ytr__arg* a, int na) {
    ytr_desc* d = rl->d;
    const ytb_table* tb = rl->r->table;
    int f = 0, l, l2, x, y, c, k;
    const char* alias;
    size_t v;
    if (a[0].v || a[0].kq) return ytr__rl_fail(rl, a[0].col, "a statement starts with a word"), false;
#define VERB(w) (a[0].kn == sizeof(w) - 1 && memcmp(a[0].k, w, sizeof(w) - 1) == 0)
    if (VERB("order")) {
        if (!ytr__rl_args(rl, na, 1, 1, "order sequential|random|full_random|constrained|list|with_replacement|latin|balanced_latin")) return false;
        if (!ytr__rl_once(rl, YTR__O_ORDER, "order")) return false;
        return ytr__rl_order_name(rl, &a[1]);
    }
    if (VERB("shuffle")) {     /* OpenSesame: shuffle the rows */
        if (na > 1) return ytr__rl_fail(rl, a[1].col, "shuffle with a column (OpenSesame) is not supported"), false;
        if (!ytr__rl_once(rl, YTR__O_ORDER, "order (or shuffle)")) return false;
        d->order = YTR_ORDER_FULL_RANDOM;
        return true;
    }
    if (VERB("reps")) {
        if (!ytr__rl_args(rl, na, 1, 1, "reps N")) return false;
        if (!ytr__rl_once(rl, YTR__O_REPS, "reps")) return false;
        return ytr__rl_cnt(rl, &a[1], 1, &d->reps);
    }
    if (VERB("cond_reps") || VERB("weight")) {
        int* cr;
        bool w = VERB("weight");
        if (!ytr__rl_once(rl, YTR__O_CREPS, "cond_reps (or weight)")) return false;
        if (w) {
            if (!ytr__rl_args(rl, na, 1, 1, "weight COLUMN (an integer column: each row's repetitions)")) return false;
            f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
            if (f == -2) return false;
            if (!tb || f < 0 || ytb_col_type(tb, f) != YTB_INTEGER)
                return ytr__rl_fail(rl, a[1].col, "weight needs an integer table column"), false;
        } else if (!ytr__rl_args(rl, na, rl->n_cond, rl->n_cond, "cond_reps N N ... (one per row)")) {
            return false;
        }
        cr = ytr__rl_creps(rl);
        if (!cr) return false;
        for (c = 0; c < rl->n_cond; c++) {
            if (w) {
                x = ytb_int(tb, c, f);
                if (x < 0) return ytr__rl_fail(rl, a[1].col, "weight: row %d has %d repetitions", c, x), false;
            } else if (!ytr__rl_cnt(rl, &a[1 + c], 0, &x)) {
                return false;
            }
            cr[c] = x;
        }
        return true;
    }
    if (VERB("where")) {
        if (!ytr__rl_args(rl, na, 1, 1, "where COLUMN=VALUE")) return false;
        if (!ytr__rl_sel(rl, &a[1], &f, &l)) return false;
        if (l == YTR_ANY_LEVEL) return ytr__rl_fail(rl, a[1].col, "where needs COLUMN=VALUE"), false;
        if (!rl->mask) {
            rl->mask = (unsigned char*)ytr__rl_alloc(rl, (size_t)(rl->n_cond > 0 ? rl->n_cond : 1));
            if (!rl->mask) return false;
            memset(rl->mask, 1, (size_t)(rl->n_cond > 0 ? rl->n_cond : 1));
        }
        for (c = 0; c < rl->n_cond; c++) {
            int lv = (f == YTR_CONDITION) ? c : (tb ? ytb_level(tb, c, f) : -1);
            if (!tb && f >= 0) {
                /* Factorial rows: last factor fastest. */
                int s2 = 1, g;
                for (g = d->n_factors - 1; g > f; g--) s2 *= d->factors[g].n_levels;
                lv = (c / s2) % d->factors[f].n_levels;
            }
            if (lv != l) rl->mask[c] = 0;
        }
        return true;
    }
    if (VERB("subset")) {
        if (!ytr__rl_args(rl, na, 1, 1, "subset K")) return false;
        if (!ytr__rl_once(rl, YTR__O_SUBSET, "subset")) return false;
        return ytr__rl_cnt(rl, &a[1], 1, &d->subset);
    }
    if (VERB("draws")) {
        if (!ytr__rl_args(rl, na, 1, 2, "draws N [weights=COLUMN]")) return false;
        if (!ytr__rl_once(rl, YTR__O_DRAWS, "draws")) return false;
        if (!ytr__rl_cnt(rl, &a[1], 1, &d->draws)) return false;
        if (na == 3) {
            double* w;
            if (!a[2].v || a[2].kq || a[2].kn != 7 || memcmp(a[2].k, "weights", 7) != 0)
                return ytr__rl_fail(rl, a[2].col, "expected weights=COLUMN"), false;
            f = ytr__rl_factor(rl, &a[2], a[2].v, a[2].vn, a[2].vq);
            if (f == -2) return false;
            if (!tb || f < 0 || ytb_col_type(tb, f) == YTB_STRING)
                return ytr__rl_fail(rl, a[2].col, "weights needs a numeric table column"), false;
            w = (double*)ytr__rl_alloc(rl, sizeof(double) * (size_t)rl->n_cond);
            if (!w) return false;
            for (c = 0; c < rl->n_cond; c++) w[c] = ytb_num(tb, c, f);
            d->weights = w;
        }
        return true;
    }
    if (VERB("list")) {
        if (na < 2) return ytr__rl_fail(rl, 0, "expected: list ROW ROW ..."), false;
        if (!rl->list) {
            rl->list = (int*)ytr__rl_alloc(rl, sizeof(int) * YTR_MAX_TRIALS);
            if (!rl->list) return false;
        }
        for (k = 1; k < na; k++) {
            if (rl->n_list >= YTR_MAX_TRIALS)
                return ytr__rl_fail(rl, a[k].col, "the list is longer than YTR_MAX_TRIALS"), false;
            if (!ytr__rl_cnt(rl, &a[k], 0, &x)) return false;
            if (x >= rl->n_cond) return ytr__rl_fail(rl, a[k].col, "row %d is not below %d", x, rl->n_cond), false;
            rl->list[rl->n_list++] = x;
        }
        return true;
    }
    if (VERB("groups")) {
        static const char* const go[] = { "sequential", "random", "latin", "balanced_latin", "list" };
        if (na < 3) return ytr__rl_fail(rl, 0, "expected: groups COLUMN blocked|alternate [sequential|random|latin|balanced_latin|list VALUE ...]"), false;
        if (!ytr__rl_once(rl, YTR__O_GROUPS, "groups")) return false;
        f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
        if (f == -2) return false;
        if (f < 0) return ytr__rl_fail(rl, a[1].col, "groups need a column, not @row"), false;
        d->groups.factor = f;
        if (ytr__rl_word(&a[2], "blocked")) d->groups.mode = YTR_GROUPS_BLOCKED;
        else if (ytr__rl_word(&a[2], "alternate")) d->groups.mode = YTR_GROUPS_ALTERNATE;
        else return ytr__rl_fail(rl, a[2].col, "expected blocked or alternate"), false;
        d->groups.order = YTR_GROUP_ORDER_SEQUENTIAL;
        d->groups.participant = rl->r->participant;
        if (na >= 4) {
            for (k = 0; k < 5; k++) if (ytr__rl_word(&a[3], go[k])) break;
            if (k == 5) return ytr__rl_fail(rl, a[3].col, "unknown group order (sequential, random, latin, balanced_latin, list)"), false;
            d->groups.order = (ytr_group_order)k;
            if (k == YTR_GROUP_ORDER_LIST) {
                int* gl;
                if (na < 5) return ytr__rl_fail(rl, 0, "groups ... list needs the values in run order"), false;
                gl = (int*)ytr__rl_alloc(rl, sizeof(int) * (size_t)(na - 4));
                if (!gl) return false;
                for (k = 4; k < na; k++) {
                    l = ytr__rl_level(rl, &a[k], f, a[k].k, a[k].kn, a[k].kq);
                    if (l == -2) return false;
                    gl[k - 4] = l;
                }
                d->groups.list = gl;
                d->groups.n_list = na - 4;
            } else if (na > 4) {
                return ytr__rl_fail(rl, a[4].col, "only a list group order takes values"), false;
            }
        }
        return true;
    }
    if (VERB("block_size") || VERB("warmup") || VERB("requeue_gap") || VERB("max_swaps")) {
        int which = VERB("block_size") ? YTR__O_BLOCK : VERB("warmup") ? YTR__O_WARMUP
                  : VERB("requeue_gap") ? YTR__O_GAP : YTR__O_SWAPS;
        int* dst = which == YTR__O_BLOCK ? &d->block_size : which == YTR__O_WARMUP ? &d->n_warmup
                 : which == YTR__O_GAP ? &d->requeue_gap : &d->max_swaps;
        char form[40];
        snprintf(form, sizeof(form), "%.*s N", (int)a[0].kn, a[0].k);
        if (!ytr__rl_args(rl, na, 1, 1, form)) return false;
        if (!ytr__rl_once(rl, which, form)) return false;
        return ytr__rl_cnt(rl, &a[1], 1, dst);
    }
    if (VERB("span_blocks")) {
        if (!ytr__rl_args(rl, na, 0, 0, "span_blocks")) return false;
        if (!ytr__rl_once(rl, YTR__O_SPAN, "span_blocks")) return false;
        d->constraints_span_blocks = true;
        return true;
    }
    if (VERB("practice")) {
        if (na != 2 && na != 4) return ytr__rl_fail(rl, 0, "expected: practice N [from COLUMN=VALUE]"), false;
        if (!ytr__rl_once(rl, YTR__O_PRACTICE, "practice")) return false;
        if (!ytr__rl_cnt(rl, &a[1], 1, &d->n_practice)) return false;
        if (na == 4) {
            int* wl;
            if (!ytr__rl_word(&a[2], "from")) return ytr__rl_fail(rl, a[2].col, "expected from"), false;
            if (!ytr__rl_once(rl, YTR__O_WLIST, "practice ... from (or warmup_conditions)")) return false;
            if (!ytr__rl_sel(rl, &a[3], &f, &l)) return false;
            if (l == YTR_ANY_LEVEL) return ytr__rl_fail(rl, a[3].col, "from needs COLUMN=VALUE"), false;
            wl = (int*)ytr__rl_alloc(rl, sizeof(int) * (size_t)rl->n_cond);
            if (!wl) return false;
            k = 0;
            for (c = 0; c < rl->n_cond; c++) {
                int lv = f == YTR_CONDITION ? c : (tb ? ytb_level(tb, c, f) : -1);
                if (!tb && f >= 0) {
                    int s2 = 1, g;
                    for (g = d->n_factors - 1; g > f; g--) s2 *= d->factors[g].n_levels;
                    lv = (c / s2) % d->factors[f].n_levels;
                }
                if (lv == l) wl[k++] = c;
            }
            if (k == 0) return ytr__rl_fail(rl, a[3].col, "no row matches"), false;
            d->warmup_conditions = wl;
            d->n_warmup_conditions = k;
        }
        return true;
    }
    if (VERB("warmup_conditions")) {
        int* wl;
        if (na < 2) return ytr__rl_fail(rl, 0, "expected: warmup_conditions ROW ROW ..."), false;
        if (!ytr__rl_once(rl, YTR__O_WLIST, "warmup_conditions (or practice ... from)")) return false;
        wl = (int*)ytr__rl_alloc(rl, sizeof(int) * (size_t)(na - 1));
        if (!wl) return false;
        for (k = 1; k < na; k++) {
            if (!ytr__rl_cnt(rl, &a[k], 0, &x)) return false;
            if (x >= rl->n_cond) return ytr__rl_fail(rl, a[k].col, "row %d is not below %d", x, rl->n_cond), false;
            wl[k - 1] = x;
        }
        d->warmup_conditions = wl;
        d->n_warmup_conditions = na - 1;
        return true;
    }
    if (VERB("max_run") || VERB("min_gap")) {
        bool run = VERB("max_run");
        if (!ytr__rl_args(rl, na, 2, 2, run ? "max_run COLUMN[=VALUE] N" : "min_gap COLUMN[=VALUE] GAP")) return false;
        if (!ytr__rl_sel(rl, &a[1], &f, &l) || !ytr__rl_cnt(rl, &a[2], 1, &x)) return false;
        return ytr__rl_add(rl, run ? ytr_max_run(f, l, x) : ytr_min_gap(f, l, x));
    }
    if (VERB("max_in_window")) {
        if (!ytr__rl_args(rl, na, 3, 3, "max_in_window COLUMN[=VALUE] WINDOW N")) return false;
        if (!ytr__rl_sel(rl, &a[1], &f, &l) || !ytr__rl_cnt(rl, &a[2], 1, &x) ||
            !ytr__rl_cnt(rl, &a[3], 1, &y))
            return false;
        return ytr__rl_add(rl, ytr_max_in_window(f, l, x, y));
    }
    if (VERB("first_not")) {
        if (!ytr__rl_args(rl, na, 1, 1, "first_not COLUMN=VALUE")) return false;
        if (!ytr__rl_sel(rl, &a[1], &f, &l)) return false;
        if (l == YTR_ANY_LEVEL) return ytr__rl_fail(rl, a[1].col, "first_not needs COLUMN=VALUE"), false;
        return ytr__rl_add(rl, ytr_first_not(f, l));
    }
    if (VERB("no_transition") || VERB("followed_by") || VERB("preceded_by")) {
        char form[64];
        snprintf(form, sizeof(form), "%.*s COLUMN VALUE VALUE", (int)a[0].kn, a[0].k);
        if (!ytr__rl_args(rl, na, 3, 3, form)) return false;
        if (a[1].v) return ytr__rl_fail(rl, a[1].col, "expected: %s", form), false;
        f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
        if (f == -2) return false;
        l = ytr__rl_level(rl, &a[2], f, a[2].k, a[2].kn, a[2].kq);
        if (l == -2) return false;
        l2 = ytr__rl_level(rl, &a[3], f, a[3].k, a[3].kn, a[3].kq);
        if (l2 == -2) return false;
        return ytr__rl_add(rl, VERB("no_transition") ? ytr_no_transition(f, l, l2)
                                 : VERB("followed_by") ? ytr_followed_by(f, l, l2) : ytr_preceded_by(f, l, l2));
    }
    if (VERB("chunk")) {
        if (!ytr__rl_args(rl, na, 1, 1, "chunk COLUMN")) return false;
        f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
        if (f == -2) return false;
        return ytr__rl_add(rl, ytr_chunk(f));
    }
    if (VERB("balance")) {
        int fl = 0;
        if (!ytr__rl_args(rl, na, 1, 3, "balance COLUMN [no_repeat] [no_leadin]")) return false;
        f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
        if (f == -2) return false;
        for (k = 2; k < na; k++) {
            if (ytr__rl_word(&a[k], "no_repeat")) fl |= YTR_BALANCE_NO_REPEAT;
            else if (ytr__rl_word(&a[k], "no_leadin")) fl |= YTR_BALANCE_NO_LEADIN;
            else return ytr__rl_fail(rl, a[k].col, "expected no_repeat or no_leadin"), false;
        }
        return ytr__rl_add(rl, ytr_balance_flags(f, fl));
    }
    if (VERB("constrain")) {
        /* OpenSesame: constrain COL maxrep=N mindist=N; mindist counts
         * rows, so 2 is "no immediate repeat", a gap of 1. */
        bool any = false;
        if (na < 3) return ytr__rl_fail(rl, 0, "expected: constrain COLUMN [maxrep=N] [mindist=N]"), false;
        if (a[1].v) return ytr__rl_fail(rl, a[1].col, "constrain takes a column, not COLUMN=VALUE"), false;
        f = ytr__rl_factor(rl, &a[1], a[1].k, a[1].kn, a[1].kq);
        if (f == -2) return false;
        for (k = 2; k < na; k++) {
            if (!a[k].v) return ytr__rl_fail(rl, a[k].col, "expected maxrep=N or mindist=N"), false;
            x = ytr__rl_count(a[k].v, a[k].vn, a[k].vq);
            if (a[k].kn == 6 && memcmp(a[k].k, "maxrep", 6) == 0) {
                if (x < 1) return ytr__rl_fail(rl, a[k].col, "maxrep must be at least 1"), false;
                if (!ytr__rl_add(rl, ytr_max_run(f, YTR_ANY_LEVEL, x))) return false;
            } else if (a[k].kn == 7 && memcmp(a[k].k, "mindist", 7) == 0) {
                if (x < 2) return ytr__rl_fail(rl, a[k].col, "mindist must be at least 2 (OpenSesame's rule)"), false;
                if (!ytr__rl_add(rl, ytr_min_gap(f, YTR_ANY_LEVEL, x - 1))) return false;
            } else {
                return ytr__rl_fail(rl, a[k].col, "expected maxrep=N or mindist=N"), false;
            }
            any = true;
        }
        return any;
    }
    if (VERB("jitter")) {
        static const char* const form =
            "jitter NAME uniform LO HI | choice V V ... | exponential LO HI SCALE, then [rate=NUM[/DEN]]";
        ytr_jitter_desc* jd;
        char* nm;
        char tok[64];
        double* vals = NULL;
        double num;
        const char* cols[3] = { NULL, NULL, NULL };
        double nums[3] = { 0.0, 0.0, 0.0 };
        int kind, np, last = na, rn = 0, rd = 0;
        if (na < 4) return ytr__rl_fail(rl, 0, "expected: %s", form), false;
        if (d->n_jitters < 0 || d->n_jitters >= YTR_MAX_JITTERS)
            return ytr__rl_fail(rl, 0, "more than %d jitters (YTR_MAX_JITTERS)", YTR_MAX_JITTERS), false;
        if (a[1].v || a[1].kq || a[1].kn < 1 || a[1].kn > YTR_MAX_JITTER_NAME)
            return ytr__rl_fail(rl, a[1].col, "a jitter's name is a word of 1 to %d characters", YTR_MAX_JITTER_NAME), false;
        if (ytr__rl_word(&a[2], "uniform")) kind = YTR_JITTER_UNIFORM;
        else if (ytr__rl_word(&a[2], "choice")) kind = YTR_JITTER_CHOICE;
        else if (ytr__rl_word(&a[2], "exponential")) kind = YTR_JITTER_EXPONENTIAL;
        else return ytr__rl_fail(rl, a[2].col, "expected uniform, choice or exponential"), false;
        if (a[na - 1].v && !a[na - 1].kq && a[na - 1].kn == 4 && memcmp(a[na - 1].k, "rate", 4) == 0) {
            const char* v2 = a[na - 1].v;
            size_t vn = a[na - 1].vn, sl;
            for (sl = 0; sl < vn && v2[sl] != '/'; sl++) {}
            rn = ytr__rl_count(v2, sl, a[na - 1].vq);
            rd = sl < vn ? ytr__rl_count(v2 + sl + 1, vn - sl - 1, a[na - 1].vq) : 1;
            if (rn < 1 || rd < 1) return ytr__rl_fail(rl, a[na - 1].col, "expected rate=NUM or rate=NUM/DEN (positive counts)"), false;
            last = na - 1;
        }
        np = last - 3;
        if (kind == YTR_JITTER_UNIFORM && np != 2) return ytr__rl_fail(rl, 0, "expected: jitter NAME uniform LO HI [rate=NUM[/DEN]]"), false;
        if (kind == YTR_JITTER_EXPONENTIAL && np != 3) return ytr__rl_fail(rl, 0, "expected: jitter NAME exponential LO HI SCALE [rate=NUM[/DEN]]"), false;
        if (kind == YTR_JITTER_CHOICE && (np < 1 || np > YTR_MAX_JITTER_VALUES))
            return ytr__rl_fail(rl, 0, "expected: jitter NAME choice V V ... (1 to %d values) [rate=NUM[/DEN]]", YTR_MAX_JITTER_VALUES), false;
        if (kind == YTR_JITTER_CHOICE) {
            vals = (double*)ytr__rl_alloc(rl, sizeof(double) * (size_t)np);
            if (!vals) return false;
        }
        for (k = 0; k < np; k++) {
            const ytr__arg* g = &a[3 + k];
            if (g->v) return ytr__rl_fail(rl, g->col, "expected a number of seconds or a column"), false;
            if (g->kn < sizeof(tok) && !g->kq) {
                memcpy(tok, g->k, g->kn);
                tok[g->kn] = '\0';
                if (ytb_parse_number(tok, g->kn, &num) == YTB_NUM_OK) {
                    if (vals) vals[k] = num;
                    else nums[k] = num;
                    continue;
                }
            }
            if (vals || !tb)
                return ytr__rl_fail(rl, g->col, "'%.*s' is not a number of seconds", (int)(g->kn > 40 ? 40 : g->kn), g->k), false;
            f = ytr__rl_factor(rl, g, g->k, g->kn, g->kq);
            if (f == -2) return false;
            if (f < 0) return ytr__rl_fail(rl, g->col, "a jitter takes a column, not @row"), false;
            cols[k] = ytb_col_name(tb, f);
        }
        nm = (char*)ytr__rl_alloc(rl, a[1].kn + 1);
        if (!nm) return false;
        memcpy(nm, a[1].k, a[1].kn);
        nm[a[1].kn] = '\0';
        jd = &d->jitters[d->n_jitters++];
        memset(jd, 0, sizeof(*jd));
        jd->name = nm;
        jd->dist = (ytr_jitter_dist)kind;
        jd->lo = nums[0];
        jd->hi = nums[1];
        jd->scale = nums[2];
        jd->lo_column = cols[0];
        jd->hi_column = cols[1];
        jd->scale_column = cols[2];
        jd->values = vals;
        jd->n_values = vals ? np : 0;
        jd->rate_num = rn;
        jd->rate_den = rd;
        return true;
    }
#undef VERB
    {
        static const char* const os[] = { "slice", "sort", "sortby", "reverse", "roll", "shuffle_horiz",
                                          "fullfactorial", "setcycle" };
        size_t i;
        int best = 99, bi = -1, dd;
        for (i = 0; i < sizeof(os) / sizeof(os[0]); i++)
            if (strlen(os[i]) == a[0].kn && memcmp(os[i], a[0].k, a[0].kn) == 0)
                return ytr__rl_fail(rl, a[0].col, "the OpenSesame operation '%s' is not supported; use where or list", os[i]), false;
        alias = ytr__rl_alias(a[0].k, a[0].kn);
        if (alias)
            return ytr__rl_fail(rl, a[0].col, "unknown statement '%.*s'; the statement here is '%s'",
                                  (int)a[0].kn, a[0].k, alias), false;
        for (v = 0; v < sizeof(ytr__rl_verbs) / sizeof(ytr__rl_verbs[0]); v++) {
            dd = ytr__rl_lev_dist(a[0].k, a[0].kn, ytr__rl_verbs[v]);
            if (dd < best) { best = dd; bi = (int)v; }
        }
        if (bi >= 0 && best <= 2)
            return ytr__rl_fail(rl, a[0].col, "unknown statement '%.*s'; did you mean '%s'?",
                                  (int)(a[0].kn > 30 ? 30 : a[0].kn), a[0].k, ytr__rl_verbs[bi]), false;
        return ytr__rl_fail(rl, a[0].col, "unknown statement '%.*s'", (int)(a[0].kn > 30 ? 30 : a[0].kn), a[0].k), false;
    }
}

YTR_API int ytr_rules(ytr_desc* d, const ytr_rules_desc* r, char* err, size_t cap) {
    ytr__rl rl;
    ytr__arg args[YTR__RL_ARGS];
    const char* p;
    size_t n, i = 0, st;
    int na, c;
    if (err && cap) err[0] = '\0';
    memset(&rl, 0, sizeof(rl));
    rl.d = d;
    rl.r = r;
    rl.err = err;
    rl.cap = cap;
    if (!d || !r || (!r->text && r->len)) {
        if (err && cap) snprintf(err, cap, "ysp_trials: rules: null desc or text");
        return 1;
    }
    if (r->participant < 0) return ytr__rl_fail(&rl, 0, "the participant number is negative");
    rl.arena = (unsigned char*)r->arena;
    rl.size = r->arena_size;
    if (r->table) {
        if (!r->table->base) return ytr__rl_fail(&rl, 0, "the table is not parsed");
        if (d->n_factors) return ytr__rl_fail(&rl, 0, "a table and desc.factors cannot both be given");
        rl.n_cond = r->table->n_rows;
        d->table = r->table;
    } else if (d->n_factors > 0 && d->n_factors <= YTR_MAX_FACTORS) {
        int f;
        rl.n_cond = 1;
        for (f = 0; f < d->n_factors; f++) {
            if (d->factors[f].n_levels < 1 || rl.n_cond > YTR_MAX_CONDITIONS) break;
            rl.n_cond *= d->factors[f].n_levels;
        }
    } else {
        rl.n_cond = d->n_conditions;
    }
    p = r->text;
    n = r->len;
    if (n >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) i = 3;
    while (i < n && !rl.failed) {
        size_t e;
        st = i;
        while (i < n && p[i] != '\n') i++;
        e = i;
        if (e > st && p[e - 1] == '\r') e--;
        rl.line++;
        if (rl.line > YTR__RL_LINES) return ytr__rl_fail(&rl, 0, "more than %d lines", YTR__RL_LINES);
        if (e - st > YTR__RL_LINE) return ytr__rl_fail(&rl, 0, "a line of more than %d bytes", YTR__RL_LINE);
        if (memchr(p + st, '\0', e - st) || memchr(p + st, '\r', e - st))
            return ytr__rl_fail(&rl, 0, "a NUL or a lone CR in the line");
        na = ytr__rl_split(&rl, p + st, e - st, args);
        if (na < 0) return rl.line;
        if (na > 0 && !ytr__rl_stmt(&rl, args, na)) return rl.line;
        if (i < n) i++;
    }
    if (rl.failed) return rl.line;
    /* Finish: where, cond_reps, the list. */
    rl.line = 0;
    if (rl.cond_reps || rl.mask) {
        int* cr = ytr__rl_creps(&rl);
        if (!cr) return 1;
        for (c = 0; c < rl.n_cond; c++) {
            if (cr[c] < 0) {
                if (d->reps < 1) return ytr__rl_fail(&rl, 0, "where needs reps (or weight, or cond_reps)");
                cr[c] = d->reps;
            }
            if (rl.mask && !rl.mask[c]) cr[c] = 0;
        }
        d->cond_reps = cr;
    }
    if (rl.list) {
        d->order_list = rl.list;
        d->n_order_list = rl.n_list;
    }
    return 0;
}

/* --- snapshot ---------------------------------------------------------- */

/* One writer for three jobs: count (out and cmp NULL), write (out), and
 * compare against a snapshot (cmp), which is how load() checks the desc
 * without a second description of the layout that could drift. */
typedef struct ytr__w {
    unsigned char*       out;
    const unsigned char* cmp;
    size_t               pos;
    size_t               cap;
    const char*          diff;   /* compare: the first field that differs */
} ytr__w;

static void ytr__put(ytr__w* w, uint64_t v, int nbytes, const char* name) {
    int i;
    unsigned char b;
    for (i = 0; i < nbytes; i++) {
        b = (unsigned char)((v >> (8 * i)) & 0xffu);
        if (w->out) {
            if (w->pos < w->cap) w->out[w->pos] = b;
        } else if (w->cmp && !w->diff) {
            if (w->pos >= w->cap || w->cmp[w->pos] != b) w->diff = name;
        }
        w->pos++;
    }
}

static void ytr__put_i32(ytr__w* w, int v, const char* name) {
    ytr__put(w, (uint64_t)(uint32_t)v, 4, name);
}

static void ytr__put_f64(ytr__w* w, double v, const char* name) {
    uint64_t u;
    memcpy(&u, &v, sizeof(u));
    ytr__put(w, u, 8, name);
}

/* The snapshot format a session saves and loads. */
static uint32_t ytr__snap_format(const ytr_trials* t) {
    return t->n_jit > 0 ? YTR__SNAP_FORMAT3 : t->v2 ? YTR__SNAP_FORMAT2 : YTR__SNAP_FORMAT;
}

static void ytr__put_desc(ytr__w* w, const ytr_trials* t) {
    const ytr_desc* d = &t->desc;
    int i;
    ytr__put_i32(w, d->n_conditions, "n_conditions");
    ytr__put_i32(w, d->n_factors, "n_factors");
    for (i = 0; i < ytr__nf(t); i++) ytr__put_i32(w, d->factors[i].n_levels, "factors[].n_levels");
    ytr__put_i32(w, d->reps, "reps");
    ytr__put(w, t->has_cond_reps ? 1u : 0u, 1, "cond_reps");
    if (t->has_cond_reps)
        for (i = 0; i < ytr__nc(t); i++) ytr__put_i32(w, t->cond_reps[i], "cond_reps[]");
    ytr__put_i32(w, (int)d->order, "order");
    ytr__put_i32(w, d->n_constraints, "n_constraints");
    for (i = 0; i < ytr__nci(t); i++) {
        const ytr_constraint* c = &d->constraints[i];
        ytr__put_i32(w, (int)c->rule, "constraints[].rule");
        ytr__put_i32(w, c->factor, "constraints[].factor");
        ytr__put_i32(w, c->level, "constraints[].level");
        ytr__put_i32(w, c->level2, "constraints[].level2");
        ytr__put_i32(w, c->n, "constraints[].n");
        ytr__put_i32(w, c->window, "constraints[].window");
    }
    ytr__put_i32(w, d->max_swaps, "max_swaps");
    ytr__put_i32(w, d->n_tracks, "n_tracks");
    for (i = 0; i < ytr__ntr(t); i++) ytr__put_f64(w, d->tracks[i].weight, "tracks[].weight");
    ytr__put_i32(w, (int)d->interleave, "interleave");
    ytr__put_f64(w, d->track_rate, "track_rate");
    ytr__put_i32(w, d->block_size, "block_size");
    ytr__put(w, d->constraints_span_blocks ? 1u : 0u, 1, "constraints_span_blocks");
    ytr__put_i32(w, d->n_practice, "n_practice");
    ytr__put_i32(w, d->n_warmup, "n_warmup");
    ytr__put_i32(w, d->n_warmup_conditions, "n_warmup_conditions");
    for (i = 0; i < ytr__nwl(t); i++)
        ytr__put_i32(w, t->warm_list[i], "warmup_conditions[]");
    ytr__put_i32(w, d->requeue_gap, "requeue_gap");
    ytr__put(w, (uint64_t)d->record_size, 8, "record_size");
    if (t->v2 || t->n_jit > 0) {
        /* Format 2: what v0.2 added. The arrays the handle does not keep
         * (weights, the order list, the group list) are checked by hash. */
        ytr__put(w, d->table ? ytb_hash(d->table) : 0u, 8, "table");
        ytr__put_i32(w, t->n_cond, "table rows");
        ytr__put_i32(w, t->n_fac, "table columns");
        ytr__put_i32(w, d->n_order_list, "n_order_list");
        ytr__put_i32(w, d->draws, "draws");
        ytr__put_i32(w, d->subset, "subset");
        ytr__put_i32(w, (int)d->groups.mode, "groups.mode");
        ytr__put_i32(w, d->groups.factor, "groups.factor");
        ytr__put_i32(w, (int)d->groups.order, "groups.order");
        ytr__put_i32(w, d->groups.participant, "groups.participant");
        ytr__put_i32(w, d->groups.n_list, "groups.n_list");
        ytr__put(w, t->aux_hash, 8, "order_list, weights or groups.list");
    }
    if (t->n_jit > 0) {
        size_t n, k;
        ytr__put_i32(w, t->n_jit, "n_jitters");
        for (i = 0; i < t->n_jit && i < YTR_MAX_JITTERS; i++) {
            const ytr__jit* q = &t->jit[i];
            n = strlen(q->name);
            ytr__put(w, (uint64_t)n, 1, "jitters[].name");
            for (k = 0; k < n; k++) ytr__put(w, (unsigned char)q->name[k], 1, "jitters[].name");
            ytr__put_i32(w, q->dist, "jitters[].dist");
            ytr__put_f64(w, q->lo, "jitters[].lo");
            ytr__put_f64(w, q->hi, "jitters[].hi");
            ytr__put_f64(w, q->scale, "jitters[].scale");
            ytr__put_i32(w, q->n_values, "jitters[].n_values");
            for (k = 0; k < (size_t)q->n_values && k < YTR_MAX_JITTER_VALUES; k++)
                ytr__put_f64(w, q->values[k], "jitters[].values");
            ytr__put_i32(w, q->rate_num, "jitters[].rate_num");
            ytr__put_i32(w, q->rate_den, "jitters[].rate_den");
            ytr__put_i32(w, q->col[0], "jitters[].lo_column");
            ytr__put_i32(w, q->col[1], "jitters[].hi_column");
            ytr__put_i32(w, q->col[2], "jitters[].scale_column");
        }
    }
}

static void ytr__put_all(ytr__w* w, const ytr_trials* t) {
    int i;
    const ytr_trial* h;
    const unsigned char* rec;
    size_t k, nrec;

    ytr__put(w, 'P', 1, "magic");
    ytr__put(w, 'S', 1, "magic");
    ytr__put(w, 'T', 1, "magic");
    ytr__put(w, 'R', 1, "magic");
    ytr__put(w, ytr__snap_format(t), 4, "format");
    ytr__put_desc(w, t);

    ytr__put_i32(w, t->n_scheduled, "n_scheduled");
    ytr__put_i32(w, t->n_run, "n_run");
    ytr__put_i32(w, t->n_done, "n_done");
    ytr__put_i32(w, t->current, "current");
    ytr__put_i32(w, t->schedule_pos, "schedule_pos");
    ytr__put_i32(w, t->rr_next, "rr_next");
    ytr__put(w, t->track_done, 4, "track_done");
    ytr__put_i32(w, t->break_pending, "break_pending");
    ytr__put_i32(w, t->n_main, "n_main");
    ytr__put_i32(w, t->seg_start, "seg_start");
    ytr__put_i32(w, t->warm_block, "warm_block");
    ytr__put_i32(w, t->warm_done, "warm_done");
    ytr__put_i32(w, t->swaps, "swaps");
    if (t->v2 || t->n_jit > 0) {
        ytr__put_i32(w, t->blk, "blk");
        ytr__put_i32(w, t->blk_next, "blk_next");
        ytr__put(w, t->units ? 1u : 0u, 1, "units");
        ytr__put(w, t->has_leadin ? 1u : 0u, 1, "has_leadin");
    }

    for (i = 0; i < ytr__nsch(t); i++)
        ytr__put(w, (uint64_t)(uint16_t)t->schedule[i], 2, "schedule");
    for (i = 0; i < ytr__nsch(t); i++)
        ytr__put(w, (uint64_t)(uint16_t)t->schedule_rep[i], 2, "schedule_rep");
    for (i = 0; i < ytr__nsch(t); i++)
        ytr__put(w, t->schedule_flags[i], 1, "schedule_flags");
    for (i = 0; i < ytr__nmain(t); i++)
        ytr__put(w, (uint64_t)(uint16_t)t->main_seq[i], 2, "main");
    for (i = 0; i < ytr__nrun(t); i++) {
        h = &t->history[i];
        ytr__put(w, (uint64_t)(uint16_t)h->condition, 2, "history");
        ytr__put(w, (uint64_t)(uint8_t)h->track, 1, "history");
        ytr__put(w, h->flags, 1, "history");
        ytr__put(w, (uint64_t)(uint16_t)h->rep, 2, "history");
        ytr__put(w, (uint64_t)(uint16_t)h->block, 2, "history");
        ytr__put(w, (uint64_t)(uint32_t)h->outcome, 4, "history");
    }
    for (i = 0; i < ytr__nc(t); i++) ytr__put_i32(w, t->tally_valid[i], "tally_valid");
    for (i = 0; i < ytr__nc(t); i++) ytr__put_i32(w, t->tally_pos[i], "tally_pos");
    if (t->desc.record_size > 0) {
        rec = (const unsigned char*)t->desc.records;
        nrec = (size_t)ytr__nrun(t) * t->desc.record_size;
        for (k = 0; k < nrec; k++) ytr__put(w, rec[k], 1, "records");
    }
    for (i = 0; i < ytr__nrun(t); i++) {
        int j;
        for (j = 0; j < t->n_jit && j < YTR_MAX_JITTERS; j++)
            ytr__put(w, (uint64_t)t->jit_raw[i][j], 8, "jitter");
    }
}

YTR_API size_t ytr_save_size(const ytr_trials* t) {
    ytr__w w;
    if (!t || !t->open) return 0;
    memset(&w, 0, sizeof(w));
    ytr__put_all(&w, t);
    return w.pos;
}

YTR_API int ytr_save(const ytr_trials* t, void* buf, size_t cap) {
    ytr__w w;
    size_t need;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    need = ytr_save_size(t);
    if (!buf || cap < need || need > 0x7fffffff) return YTR_ERR_ARG;
    memset(&w, 0, sizeof(w));
    w.out = (unsigned char*)buf;
    w.cap = cap;
    ytr__put_all(&w, t);
    return (int)w.pos;
}

typedef struct ytr__r {
    const unsigned char* in;
    size_t               pos;
    size_t               len;
    bool                 bad;
} ytr__r;

static uint64_t ytr__get(ytr__r* r, int nbytes) {
    uint64_t v = 0;
    int i;
    if (r->bad || r->len - r->pos < (size_t)nbytes) {
        r->bad = true;
        return 0;
    }
    for (i = 0; i < nbytes; i++) v |= (uint64_t)r->in[r->pos + (size_t)i] << (8 * i);
    r->pos += (size_t)nbytes;
    return v;
}

static int ytr__get_i32(ytr__r* r) { return (int)(int32_t)(uint32_t)ytr__get(r, 4); }
static int ytr__get_i16(ytr__r* r) { return (int)(int16_t)(uint16_t)ytr__get(r, 2); }

YTR_API bool ytr_load(ytr_trials* t, const ytr_desc* desc, const void* buf, size_t len) {
    ytr__w w;
    ytr__r r;
    const unsigned char* in = (const unsigned char*)buf;
    int i, v, nt;
    ytr_trial* h;
    unsigned char* rec;
    size_t k, nrec;

    if (!t) return false;
    if (!ytr__setup(t, desc)) return false;
    if (!in) return ytr__fail(t, "ytr_load: null snapshot");
    if (len < 8 || in[0] != 'P' || in[1] != 'S' || in[2] != 'T' || in[3] != 'R')
        return ytr__fail(t, "ytr_load: not a ysp_trials snapshot");
    memset(&r, 0, sizeof(r));
    r.in = in;
    r.len = len;
    r.pos = 4;
    if ((uint32_t)ytr__get(&r, 4) != ytr__snap_format(t))
        return ytr__fail(t, "ytr_load: snapshot format is not %u", (unsigned)ytr__snap_format(t));

    memset(&w, 0, sizeof(w));
    w.cmp = in;
    w.cap = len;
    w.pos = 8;
    ytr__put_desc(&w, t);
    if (w.diff)
        return ytr__fail(t, "ytr_load: desc.%s does not match the snapshot", w.diff);
    r.pos = w.pos;

    t->n_scheduled  = ytr__get_i32(&r);
    t->n_run        = ytr__get_i32(&r);
    t->n_done       = ytr__get_i32(&r);
    t->current      = ytr__get_i32(&r);
    t->schedule_pos = ytr__get_i32(&r);
    t->rr_next      = ytr__get_i32(&r);
    t->track_done   = (uint32_t)ytr__get(&r, 4);
    t->break_pending = ytr__get_i32(&r);
    t->n_main       = ytr__get_i32(&r);
    t->seg_start    = ytr__get_i32(&r);
    t->warm_block   = ytr__get_i32(&r);
    t->warm_done    = ytr__get_i32(&r);
    t->swaps        = ytr__get_i32(&r);
    if (t->v2 || t->n_jit > 0) {
        t->blk        = ytr__get_i32(&r);
        t->blk_next   = ytr__get_i32(&r);
        t->units      = ytr__get(&r, 1) != 0;
        t->has_leadin = ytr__get(&r, 1) != 0;
        if (t->blk < -1 || t->blk_next < 0) r.bad = true;
    }
    nt = t->desc.n_tracks;
    if (r.bad ||
        t->n_scheduled < t->desc.n_practice || t->n_scheduled > YTR_MAX_TRIALS ||
        t->n_run < 0 || t->n_run > YTR_MAX_TRIALS ||
        t->n_done < 0 || t->n_done > t->n_run ||
        (t->current != -1 && t->current != t->n_run - 1) ||
        t->n_done != t->n_run - (t->current >= 0 ? 1 : 0) ||
        t->schedule_pos < 0 || t->schedule_pos > t->n_scheduled ||
        t->rr_next < 0 || t->rr_next >= (nt > 0 ? nt : 1) ||
        (nt < 32 && (t->track_done >> nt) != 0) ||
        t->break_pending < 0 || t->break_pending > 3 ||
        t->n_main < 0 || t->n_main > t->n_run ||
        t->seg_start < 0 || t->seg_start > t->n_main ||
        t->warm_done < 0 || t->warm_done > t->desc.n_warmup || t->swaps < 0)
        return ytr__fail(t, "ytr_load: the snapshot's counters are corrupt or truncated");

    for (i = 0; i < ytr__nsch(t); i++) {
        v = ytr__get_i16(&r);
        if (v < 0 || v >= t->n_cond) r.bad = true;
        t->schedule[i] = (int16_t)v;
    }
    for (i = 0; i < ytr__nsch(t); i++) t->schedule_rep[i] = (int16_t)ytr__get_i16(&r);
    for (i = 0; i < ytr__nsch(t); i++) t->schedule_flags[i] = (uint8_t)ytr__get(&r, 1);
    for (i = 0; i < ytr__nmain(t); i++) {
        v = ytr__get_i16(&r);
        if (v < -1 || v >= t->n_cond) r.bad = true;
        t->main_seq[i] = (int16_t)v;
    }
    for (i = 0; i < ytr__nrun(t); i++) {
        h = &t->history[i];
        v = ytr__get_i16(&r);
        if (v < -1 || v >= t->n_cond) r.bad = true;
        h->condition = (int16_t)v;
        v = (int)(int8_t)(uint8_t)ytr__get(&r, 1);
        if (v < -1 || v >= nt || (v < 0) == (h->condition < 0)) r.bad = true;
        h->track = (int8_t)v;
        h->flags = (uint8_t)ytr__get(&r, 1);
        h->rep = (int16_t)ytr__get_i16(&r);
        h->block = (int16_t)ytr__get_i16(&r);
        h->outcome = (int32_t)ytr__get_i32(&r);
    }
    for (i = 0; i < ytr__nc(t); i++) {
        t->tally_valid[i] = ytr__get_i32(&r);
        if (t->tally_valid[i] < 0) r.bad = true;
    }
    for (i = 0; i < ytr__nc(t); i++) {
        t->tally_pos[i] = ytr__get_i32(&r);
        if (t->tally_pos[i] < 0 || t->tally_pos[i] > t->tally_valid[i]) r.bad = true;
    }
    if (t->desc.record_size > 0) {
        nrec = (size_t)ytr__nrun(t) * t->desc.record_size;
        if (r.bad || r.len - r.pos < nrec) {
            r.bad = true;
        } else {
            rec = (unsigned char*)t->desc.records;
            for (k = 0; k < nrec; k++) rec[k] = r.in[r.pos + k];
            r.pos += nrec;
        }
    }
    for (i = 0; i < ytr__nrun(t); i++) {
        int j;
        for (j = 0; j < t->n_jit && j < YTR_MAX_JITTERS; j++) {
            /* A draw is nanoseconds in [0, the longest duration], or a
             * frame count no larger than that duration's; anything else is
             * corruption. */
            int64_t x = (int64_t)ytr__get(&r, 8);
            double lim = YTR_JITTER_MAX_S * 1e9;
            if (t->jit[j].rate_num > 0) lim = YTR_JITTER_MAX_S * t->jit[j].rate_num / t->jit[j].rate_den + 1.0;
            if (x < 0 || (double)x > lim) r.bad = true;
            else t->jit_raw[i][j] = x;
        }
    }
    if (r.bad)
        return ytr__fail(t, "ytr_load: the snapshot is corrupt or truncated");
    if (r.pos != r.len)
        return ytr__fail(t, "ytr_load: %lu bytes after the snapshot's end",
                           (unsigned long)(r.len - r.pos));

    t->desc.weights = NULL;
    t->error[0] = '\0';
    t->open = true;
    return true;
}

YTR_API int ytr_restore(ytr_trials* t, const int* outcomes, const void* records, int n) {
    const unsigned char* rec = (const unsigned char*)records;
    int i, rc;
    if (!t) return YTR_ERR_ARG;
    if (!t->open) return YTR_ERR_CLOSED;
    if (n < 0 || (n > 0 && !outcomes)) return YTR_ERR_ARG;
    if (t->n_run != 0) return YTR_ERR_ORDER;
    for (i = 0; i < n; i++) {
        rc = ytr_next(t, NULL);
        if (rc == YTR_DONE) return YTR_ERR_ARG;
        if (rc < 0) return rc;
        if (outcomes[i] == YTR_REQUEUE) {
            /* A refused re-queue recorded YTR_INVALID in the original run
             * too, so FULL here is the same state, not a failure. */
            rc = ytr_requeue(t);
            if (rc < 0 && rc != YTR_ERR_FULL) return rc;
        } else {
            rc = ytr_update(t, outcomes[i],
                              rec ? rec + (size_t)i * t->desc.record_size : NULL);
            if (rc < 0) return rc;
        }
    }
    return 0;
}

/* --- data file (v0.3.0) ------------------------------------------------ */

static bool ytr__dfail(ytr_data* w, const char* fmt, ...) {
    va_list ap;
    int n = snprintf(w->error, sizeof(w->error), "ysp_trials: ");
    if (n < 0) n = 0;
    va_start(ap, fmt);
    vsnprintf(w->error + n, sizeof(w->error) - (size_t)n, fmt, ap);
    va_end(ap);
    return false;
}

/* The length of the valid UTF-8 sequence at p, or 0: ysp/table.h's rules
 * (no overlong forms, no surrogates, nothing above U+10FFFF), so every
 * text written reads back. */
static size_t ytr__u8(const unsigned char* p, size_t n) {
    unsigned c = p[0];
    if (c < 0x80) return 1;
    if (c >= 0xC2 && c <= 0xDF) return (n >= 2 && (p[1] & 0xC0u) == 0x80u) ? 2 : 0;
    if (c >= 0xE0 && c <= 0xEF) {
        if (n < 3 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u) return 0;
        if ((c == 0xE0 && p[1] < 0xA0) || (c == 0xED && p[1] > 0x9F)) return 0;
        return 3;
    }
    if (c >= 0xF0 && c <= 0xF4) {
        if (n < 4 || (p[1] & 0xC0u) != 0x80u || (p[2] & 0xC0u) != 0x80u ||
            (p[3] & 0xC0u) != 0x80u)
            return 0;
        if ((c == 0xF0 && p[1] < 0x90) || (c == 0xF4 && p[1] > 0x8F)) return 0;
        return 4;
    }
    return 0;
}

/* The buffer to the file or the write function. After a failure nothing
 * more is written: a row with a hole in it would misalign the columns. */
static void ytr__dspill(ytr_data* w) {
    if (w->len == 0) return;
    if (!w->io_failed) {
        bool ok;
        if (w->file) ok = fwrite(w->buf, 1, w->len, (FILE*)w->file) == w->len;
        else ok = w->write(w->write_ctx, w->buf, w->len);
        if (!ok) {
            w->io_failed = true;
            ytr__dfail(w, "ytr_data_write: the write failed after %lld rows (errno %d)",
                         (long long)w->rows, errno);
        }
    }
    w->len = 0;
}

static void ytr__dput(ytr_data* w, const char* s, size_t n) {
    while (n > 0) {
        size_t room = sizeof(w->buf) - w->len, k = n < room ? n : room;
        memcpy(w->buf + w->len, s, k);
        w->len += k;
        s += k;
        n -= k;
        if (w->len == sizeof(w->buf)) ytr__dspill(w);
    }
}

static void ytr__dputc(ytr_data* w, char c) {
    if (w->len == sizeof(w->buf)) ytr__dspill(w);
    w->buf[w->len++] = c;
}

static size_t ytr__u64_text(char* out, uint64_t v) {
    char tmp[20];
    size_t n = 0, i;
    do {
        tmp[n++] = (char)('0' + (int)(v % 10u));
        v /= 10u;
    } while (v);
    for (i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    return n;
}

static void ytr__dput_i64(ytr_data* w, int64_t v) {
    char b[24];
    size_t n = 0;
    uint64_t u = (uint64_t)v;
    if (v < 0) {
        b[n++] = '-';
        u = (uint64_t)0 - u;
    }
    n += ytr__u64_text(b + n, u);
    ytr__dput(w, b, n);
}

/* ns as seconds with `dec` decimals (1..9), rounded half away from zero in
 * integers, so 9 decimals is the exact value and no double is involved. */
static void ytr__dput_time(ytr_data* w, int64_t ns, int dec) {
    char b[40];
    size_t n = 0, k;
    uint64_t u = (uint64_t)ns, scale = 1, p10 = 1, frac;
    int i;
    bool neg = ns < 0;
    if (neg) u = (uint64_t)0 - u;
    for (i = dec; i < 9; i++) scale *= 10u;
    for (i = 0; i < dec; i++) p10 *= 10u;
    u = u / scale + (u % scale >= (scale + 1u) / 2u && scale > 1u ? 1u : 0u);
    if (neg && u != 0) b[n++] = '-';
    n += ytr__u64_text(b + n, u / p10);
    b[n++] = '.';
    frac = u % p10;
    for (k = (size_t)dec; k > 0; k--) {
        b[n + k - 1] = (char)('0' + (int)(frac % 10u));
        frac /= 10u;
    }
    n += (size_t)dec;
    ytr__dput(w, b, n);
}

/* A finite double: `dec` fixed decimals, or (0) the fewest of 15 to 17
 * significant digits that read back as x through ysp/table.h. '.' for the
 * decimal point whatever LC_NUMERIC says. NaN and infinities are missing:
 * ysp/table.h refuses inf and nan. */
static void ytr__dput_double(ytr_data* w, double x, int dec) {
    char b[400];
    double y;
    int prec, n = 0, i;
    if (x != x || x - x != 0.0) return;
    if (dec > 0) {
        n = snprintf(b, sizeof(b), "%.*f", dec, x);
    } else {
        for (prec = 15; prec <= 17; prec++) {
            n = snprintf(b, sizeof(b), "%.*g", prec, x);
            for (i = 0; i < n; i++)
                if (!((b[i] >= '0' && b[i] <= '9') || b[i] == 'e' || b[i] == '-' || b[i] == '+'))
                    b[i] = '.';
            if (ytb_parse_number(b, (size_t)n, &y) == YTB_NUM_OK && y == x) break;
        }
    }
    if (n <= 0 || n >= (int)sizeof(b)) return;
    for (i = 0; i < n; i++)
        if (!((b[i] >= '0' && b[i] <= '9') || b[i] == 'e' || b[i] == '-' || b[i] == '+')) b[i] = '.';
    ytr__dput(w, b, (size_t)n);
}

/* A text field: quoted when it holds a comma, a quote or a line break
 * (RFC 4180, quotes doubled); a byte that is not valid UTF-8 becomes
 * U+FFFD, so the file stays readable. */
static void ytr__dput_text(ytr_data* w, const char* s, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    size_t i = 0, run = 0, k;
    bool quote = false;
    for (k = 0; k < n; k++)
        if (p[k] == ',' || p[k] == '"' || p[k] == '\n' || p[k] == '\r') { quote = true; break; }
    if (quote) ytr__dputc(w, '"');
    while (i < n) {
        k = ytr__u8(p + i, n - i);
        if (k == 0 || p[i] == '"') {
            ytr__dput(w, s + run, i - run);
            if (k == 0) {
                ytr__dput(w, "\xEF\xBF\xBD", 3);
                i++;
            } else {
                ytr__dput(w, "\"\"", 2);
                i += k;
            }
            run = i;
        } else {
            i += k;
        }
    }
    ytr__dput(w, s + run, n - run);
    if (quote) ytr__dputc(w, '"');
}

/* ysp/table.h's column-name rule, so the header reads back. */
static bool ytr__dname_ok(const char* s, size_t n) {
    size_t i;
    if (n == 0 || n > YTB_MAX_NAME) return false;
    for (i = 0; i < n; i++) {
        char c = s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (i > 0 && c >= '0' && c <= '9')))
            return false;
    }
    return true;
}

/* The longest line ytr_format_row() can give this session: ten integers,
 * a lead-in flag, the factors (a table cell quoted, every byte a quote at
 * worst) and the jitters' seconds. */
static size_t ytr__row_bound(const ytr_trials* t) {
    size_t n = 12u * 12u, longest;
    int f, l, nl;
    for (f = 0; f < ytr__nfe(t); f++) {
        if (t->desc.table) {
            longest = 0;
            nl = ytb_n_levels(t->desc.table, f);
            for (l = 0; l < nl; l++) {
                const char* s = ytb_level_text(t->desc.table, f, l);
                size_t k = s ? strlen(s) : 0;
                if (k > longest) longest = k;
            }
            n += 2u * longest + 3u;
        } else {
            n += 12u;
        }
    }
    return n + 24u * (size_t)(t->n_jit > 0 ? t->n_jit : 0) + 2u;
}

static FILE* ytr__fopen(const char* path, bool append) {
    FILE* f = NULL;
#ifdef _WIN32
    /* UTF-8 to UTF-16 for _wfopen, so a path with accents works. */
    wchar_t wp[1024];
    const unsigned char* p = (const unsigned char*)path;
    size_t n = strlen(path), i = 0, k = 0;
    while (i < n) {
        size_t m = ytr__u8(p + i, n - i);
        uint32_t cp;
        if (m == 0 || k + 3 > sizeof(wp) / sizeof(wp[0])) return NULL;
        if (m == 1) cp = p[i];
        else if (m == 2) cp = (p[i] & 0x1Fu) << 6 | (p[i + 1] & 0x3Fu);
        else if (m == 3) cp = (p[i] & 0x0Fu) << 12 | (p[i + 1] & 0x3Fu) << 6 | (p[i + 2] & 0x3Fu);
        else cp = (p[i] & 0x07u) << 18 | (p[i + 1] & 0x3Fu) << 12 | (p[i + 2] & 0x3Fu) << 6 |
                  (p[i + 3] & 0x3Fu);
        i += m;
        if (cp >= 0x10000u) {
            cp -= 0x10000u;
            wp[k++] = (wchar_t)(0xD800u + (cp >> 10));
            wp[k++] = (wchar_t)(0xDC00u + (cp & 0x3FFu));
        } else {
            wp[k++] = (wchar_t)cp;
        }
    }
    wp[k] = 0;
    /* Others may read the file while the session writes it (a monitor, a
     * copy); _wfopen_s would deny them. */
    f = _wfsopen(wp, append ? L"ab" : L"wb", _SH_DENYWR);
#else
    f = fopen(path, append ? "ab" : "wb");
#endif
    return f;
}

/* Hand the file's bytes to the disk (YTR_FLUSH_SYNC). */
static bool ytr__fsync(FILE* f) {
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

static int ytr__dtype_size(int type) {
    switch (type) {
    case YTR_COL_INT:     return (int)sizeof(int);
    case YTR_COL_INT64:   return 8;
    case YTR_COL_DOUBLE:  return (int)sizeof(double);
    case YTR_COL_BOOL:    return (int)sizeof(bool);
    case YTR_COL_STRING:  return (int)sizeof(const char*);
    case YTR_COL_TIME_NS: return 8;
    default:              return 0;
    }
}

YTR_API bool ytr_data_open(ytr_data* w, const ytr_data_desc* desc) {
    static const char* const tnames[] = { "?", "INT", "INT64", "DOUBLE", "BOOL", "STRING",
                                          "CHARS", "TIME_NS", "CONST" };
    char names[YTR_DATA_MAX_COLS][YTB_MAX_NAME + 1];
    int n_names = 0, i, j;
    if (!w) return false;
    memset(w, 0, sizeof(*w));
    if (!desc) return ytr__dfail(w, "ytr_data_open: null desc");
    if (!desc->path && !desc->write) return ytr__dfail(w, "ytr_data_open: no path and no write function");
    if (desc->n_cols < 0 || desc->n_cols > YTR_DATA_MAX_COLS || (desc->n_cols > 0 && !desc->cols))
        return ytr__dfail(w, "ytr_data_open: n_cols %d without cols, or above %d", desc->n_cols,
                            YTR_DATA_MAX_COLS);
    if ((int)desc->flush < (int)YTR_FLUSH_ROW || (int)desc->flush > (int)YTR_FLUSH_SYNC)
        return ytr__dfail(w, "ytr_data_open: flush %d is not a ytr_flush", (int)desc->flush);
    if (desc->meta_path && strlen(desc->meta_path) >= sizeof(w->meta_path))
        return ytr__dfail(w, "ytr_data_open: meta_path is longer than %d bytes",
                            (int)sizeof(w->meta_path) - 1);

    /* The session's names, from its header line. */
    if (desc->trials) {
        int len;
        char* p;
        if (!ytr_is_open(desc->trials)) return ytr__dfail(w, "ytr_data_open: the session is not open");
        if (ytr__row_bound(desc->trials) >= sizeof(w->buf))
            return ytr__dfail(w, "ytr_data_open: a trial's columns can take %lu bytes, more than "
                                   "YTR_DATA_BUF (%d); define it larger",
                                (unsigned long)ytr__row_bound(desc->trials), YTR_DATA_BUF);
        len = ytr_format_header(desc->trials, w->buf, sizeof(w->buf));
        if (len <= 0 || (size_t)len >= sizeof(w->buf))
            return ytr__dfail(w, "ytr_data_open: the session's header does not fit YTR_DATA_BUF");
        p = w->buf;
        while (*p && *p != '\n') {
            size_t k = strcspn(p, ",\n");
            if (n_names >= YTR_DATA_MAX_COLS)
                return ytr__dfail(w, "ytr_data_open: more than %d columns", YTR_DATA_MAX_COLS);
            if (!ytr__dname_ok(p, k))
                return ytr__dfail(w, "ytr_data_open: the session's column '%.*s' is not a name "
                                       "ysp/table.h reads ([A-Za-z_][A-Za-z0-9_]*, at most %d bytes)",
                                    (int)(k > 80 ? 80 : k), p, YTB_MAX_NAME);
            memcpy(names[n_names], p, k);
            names[n_names++][k] = '\0';
            p += k;
            if (*p == ',') p++;
        }
        w->len = (size_t)len - 1u;  /* the header line, without its newline */
        w->trials = desc->trials;
        w->next_trial = ytr_n_done(desc->trials);
    }

    for (i = 0; i < desc->n_cols; i++) {
        const ytr_col* c = &desc->cols[i];
        ytr__dcol* d = &w->col[i];
        int tsz;
        size_t k = c->name ? strlen(c->name) : 0;
        if (!c->name || !ytr__dname_ok(c->name, k))
            return ytr__dfail(w, "ytr_data_open: cols[%d]: '%s' is not a name ysp/table.h reads "
                                   "([A-Za-z_][A-Za-z0-9_]*, at most %d bytes)",
                                i, c->name ? c->name : "(null)", YTB_MAX_NAME);
        if ((int)c->type < (int)YTR_COL_INT || (int)c->type > (int)YTR_COL_CONST)
            return ytr__dfail(w, "ytr_data_open: column '%s': type %d is not a ytr_col_type", c->name,
                                (int)c->type);
        if (n_names >= YTR_DATA_MAX_COLS)
            return ytr__dfail(w, "ytr_data_open: more than %d columns", YTR_DATA_MAX_COLS);
        memcpy(names[n_names], c->name, k + 1u);
        n_names++;
        memcpy(d->name, c->name, k + 1u);
        d->type = (int)c->type;
        d->offset = c->offset;
        d->size = c->size;
        d->decimals = c->decimals;
        tsz = ytr__dtype_size(d->type);
        if (c->type == YTR_COL_CONST) {
            size_t tl;
            if (!c->text) return ytr__dfail(w, "ytr_data_open: column '%s': CONST without text", c->name);
            tl = strlen(c->text);
            /* 3 bytes per input byte at worst (U+FFFD), plus the quotes; it
             * must not spill, since no file is open yet. */
            if (tl > (sizeof(w->pool) - w->pool_len - 2u) / 3u ||
                w->len + 3u * tl + 2u >= sizeof(w->buf))
                return ytr__dfail(w, "ytr_data_open: column '%s': the CONST texts exceed %d bytes",
                                    c->name, YTR_DATA_POOL / 3);
            /* Quoted and checked once, here, through the row buffer's
             * tail, which holds nothing yet. */
            {
                size_t keep = w->len;
                ytr__dput_text(w, c->text, tl);
                d->text_off = (int)w->pool_len;
                d->text_len = (int)(w->len - keep);
                memcpy(w->pool + w->pool_len, w->buf + keep, w->len - keep);
                w->pool_len += w->len - keep;
                w->len = keep;
            }
            d->offset = YTR_UNBOUND;
            continue;
        }
        if (c->type == YTR_COL_DOUBLE ? (c->decimals < 0 || c->decimals > 17)
            : c->type == YTR_COL_TIME_NS ? (c->decimals < 0 || c->decimals > 9) : c->decimals != 0)
            return ytr__dfail(w, "ytr_data_open: column '%s': decimals %d (DOUBLE 0..17, TIME_NS 0..9, "
                                   "else 0)", c->name, c->decimals);
        if (c->type == YTR_COL_TIME_NS && d->decimals == 0) d->decimals = 9;
        if (c->offset == YTR_UNBOUND) continue;
        if (c->type == YTR_COL_CHARS ? c->size == 0 : (c->size != 0 && c->size != (size_t)tsz))
            return ytr__dfail(w, "ytr_data_open: column '%s' is %s but its field is %lu bytes%s",
                                c->name, tnames[d->type], (unsigned long)c->size,
                                c->type == YTR_COL_CHARS ? " (CHARS needs the array's size)" : "");
        if (d->size == 0) d->size = (size_t)tsz;
    }
    w->n_cols = desc->n_cols;
    if (n_names == 0) return ytr__dfail(w, "ytr_data_open: no columns (no session and no cols)");
    for (i = 0; i < n_names; i++)
        for (j = 0; j < i; j++)
            if (strcmp(names[i], names[j]) == 0)
                return ytr__dfail(w, "ytr_data_open: two columns are named '%s'", names[i]);

    if (desc->path) {
        FILE* f = ytr__fopen(desc->path, false);
        if (!f) {
            w->len = 0;
            return ytr__dfail(w, "ytr_data_open: cannot create '%s' (errno %d)", desc->path, errno);
        }
        /* No stdio buffer: a row goes from the row buffer to the OS in one
         * write, and nothing waits in a second buffer for a crash to lose. */
        setvbuf(f, NULL, _IONBF, 0);
        w->file = f;
    } else {
        w->write = desc->write;
        w->write_ctx = desc->write_ctx;
    }
    w->flush = desc->flush;
    /* The header line; a session's columns are in the buffer already. */
    for (i = 0; i < w->n_cols; i++) {
        if (i > 0 || w->trials) ytr__dputc(w, ',');
        ytr__dput(w, w->col[i].name, strlen(w->col[i].name));
    }
    ytr__dputc(w, '\n');
    ytr__dspill(w);
    if (w->io_failed) {
        if (w->file) fclose((FILE*)w->file);
        w->file = NULL;
        return ytr__dfail(w, "ytr_data_open: cannot write the header (errno %d)", errno);
    }

    if (desc->meta_path) {
        FILE* m = ytr__fopen(desc->meta_path, false);
        bool ok;
        size_t ml;
        if (!m) {
            if (w->file) fclose((FILE*)w->file);
            w->file = NULL;
            return ytr__dfail(w, "ytr_data_open: cannot create '%s' (errno %d)", desc->meta_path, errno);
        }
        ok = fprintf(m, "ysp_trials=%s data=%s\n", YTR_VERSION_STRING,
                     desc->path ? desc->path : "-") > 0;
        if (desc->meta && (ml = strlen(desc->meta)) > 0) {
            ok = fwrite(desc->meta, 1, ml, m) == ml && ok;
            if (desc->meta[ml - 1] != '\n') ok = fputc('\n', m) != EOF && ok;
        }
        if (desc->trials) {
            int len = ytr_format_meta(desc->trials, w->buf, sizeof(w->buf));
            if (len > 0 && (size_t)len < sizeof(w->buf))
                ok = fwrite(w->buf, 1, (size_t)len, m) == (size_t)len && ok;
            else  /* say so, rather than leave the reader to guess */
                ok = fprintf(m, "ysp_trials_meta=omitted (%d bytes, over YTR_DATA_BUF)\n", len) > 0 && ok;
        }
        ok = fclose(m) == 0 && ok;
        if (!ok) {
            if (w->file) fclose((FILE*)w->file);
            w->file = NULL;
            return ytr__dfail(w, "ytr_data_open: cannot write '%s'", desc->meta_path);
        }
        memcpy(w->meta_path, desc->meta_path, strlen(desc->meta_path) + 1u);
    }
    w->error[0] = '\0';
    w->open = true;
    return true;
}

YTR_API const char* ytr_data_error(const ytr_data* w) { return w ? w->error : ""; }

YTR_API int ytr_data_col(const ytr_data* w, const char* name) {
    int i;
    if (!w || !w->open || !name) return -1;
    for (i = 0; i < w->n_cols && i < YTR_DATA_MAX_COLS; i++)
        if (strcmp(w->col[i].name, name) == 0) return i;
    return -1;
}

/* The column to set, when it is a set-per-row column of one of the types. */
static ytr__dcol* ytr__dset(ytr_data* w, int col, int type_a, int type_b, int* rc) {
    ytr__dcol* d;
    *rc = 0;
    if (!w) { *rc = YTR_ERR_ARG; return NULL; }
    if (!w->open) { *rc = YTR_ERR_CLOSED; return NULL; }
    if (col < 0 || col >= w->n_cols || col >= YTR_DATA_MAX_COLS) { *rc = YTR_ERR_ARG; return NULL; }
    d = &w->col[col];
    if (d->offset != YTR_UNBOUND || (d->type != type_a && d->type != type_b)) {
        *rc = YTR_ERR_ARG;
        return NULL;
    }
    d->set = true;
    return d;
}

YTR_API int ytr_data_set_int(ytr_data* w, int col, int v) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_INT, YTR_COL_INT, &rc);
    if (d) d->v.i = v;
    return rc;
}

YTR_API int ytr_data_set_int64(ytr_data* w, int col, int64_t v) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_INT64, YTR_COL_INT64, &rc);
    if (d) d->v.i = v;
    return rc;
}

YTR_API int ytr_data_set_double(ytr_data* w, int col, double v) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_DOUBLE, YTR_COL_DOUBLE, &rc);
    if (d) d->v.d = v;
    return rc;
}

YTR_API int ytr_data_set_bool(ytr_data* w, int col, bool v) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_BOOL, YTR_COL_BOOL, &rc);
    if (d) d->v.i = v ? 1 : 0;
    return rc;
}

YTR_API int ytr_data_set_string(ytr_data* w, int col, const char* v) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_STRING, YTR_COL_CHARS, &rc);
    if (d) d->v.s = v;
    return rc;
}

YTR_API int ytr_data_set_time_ns(ytr_data* w, int col, int64_t ns) {
    int rc;
    ytr__dcol* d = ytr__dset(w, col, YTR_COL_TIME_NS, YTR_COL_TIME_NS, &rc);
    if (d) d->v.i = ns;
    return rc;
}

/* One caller field from the row struct or the value set. */
static void ytr__dfield(ytr_data* w, const ytr__dcol* d, const unsigned char* row) {
    int64_t i64 = 0;
    double x = 0.0;
    const char* s = NULL;
    size_t sl = 0;
    bool have = true;
    if (d->type == YTR_COL_CONST) {
        ytr__dput(w, w->pool + d->text_off, (size_t)d->text_len);
        return;
    }
    if (d->offset != YTR_UNBOUND) {
        const unsigned char* p = row + d->offset;
        switch (d->type) {
        case YTR_COL_INT: { int v; memcpy(&v, p, sizeof(v)); i64 = v; have = v != YTR_MISSING_INT; break; }
        case YTR_COL_BOOL: { bool v; memcpy(&v, p, sizeof(v)); i64 = v ? 1 : 0; break; }
        case YTR_COL_DOUBLE: memcpy(&x, p, sizeof(x)); break;
        case YTR_COL_STRING: memcpy(&s, p, sizeof(s)); have = s != NULL; break;
        case YTR_COL_CHARS: {
            const void* z = memchr(p, 0, d->size);
            s = (const char*)p;
            sl = z ? (size_t)((const unsigned char*)z - p) : d->size;
            break;
        }
        default: memcpy(&i64, p, sizeof(i64)); break;  /* INT64, TIME_NS */
        }
    } else if (d->set) {
        i64 = d->v.i;
        x = d->v.d;
        s = d->v.s;
        if (d->type == YTR_COL_INT && i64 == YTR_MISSING_INT) have = false;
        if ((d->type == YTR_COL_STRING || d->type == YTR_COL_CHARS) && !s) have = false;
    } else {
        have = false;
    }
    if (!have) return;
    switch (d->type) {
    case YTR_COL_INT:
    case YTR_COL_BOOL: ytr__dput_i64(w, i64); break;
    case YTR_COL_INT64: if (i64 != YTR_MISSING_I64) ytr__dput_i64(w, i64); break;
    case YTR_COL_TIME_NS: if (i64 != YTR_MISSING_NS) ytr__dput_time(w, i64, d->decimals); break;
    case YTR_COL_DOUBLE: ytr__dput_double(w, x, d->decimals); break;
    default:
        if (d->offset == YTR_UNBOUND || d->type == YTR_COL_STRING) sl = strlen(s);
        ytr__dput_text(w, s, sl);
        break;
    }
}

YTR_API int ytr_data_write(ytr_data* w, const void* row) {
    int i, last = -1;
    if (!w) return YTR_ERR_ARG;
    if (!w->open) return YTR_ERR_CLOSED;
    if (w->io_failed) return YTR_ERR_IO;
    if (!row)
        for (i = 0; i < w->n_cols && i < YTR_DATA_MAX_COLS; i++)
            if (w->col[i].offset != YTR_UNBOUND) return YTR_ERR_ARG;
    if (w->trials) {
        int n, room;
        if (!ytr_is_open(w->trials)) return YTR_ERR_CLOSED;
        /* Between update() and the next next() only: a write before
         * update() would pair this trial's values with the trial before. */
        last = ytr_n_done(w->trials) - 1;
        if (ytr_n_run(w->trials) != last + 1 || last < w->next_trial) return YTR_ERR_ORDER;
        /* The bound checked at open fits an empty buffer. */
        room = (int)(sizeof(w->buf) - w->len);
        n = ytr_format_row(w->trials, last, w->buf + w->len, (size_t)room);
        if (n >= room) {
            ytr__dspill(w);
            n = ytr_format_row(w->trials, last, w->buf, sizeof(w->buf));
        }
        if (n <= 0 || n >= (int)sizeof(w->buf)) return YTR_ERR_ARG;
        w->len += (size_t)n - 1u;  /* without its newline */
    }
    for (i = 0; i < w->n_cols && i < YTR_DATA_MAX_COLS; i++) {
        if (i > 0 || w->trials) ytr__dputc(w, ',');
        ytr__dfield(w, &w->col[i], (const unsigned char*)row);
        w->col[i].set = false;
    }
    ytr__dputc(w, '\n');
    ytr__dspill(w);
    if (w->file && w->flush == YTR_FLUSH_SYNC && !w->io_failed && !ytr__fsync((FILE*)w->file)) {
        w->io_failed = true;
        ytr__dfail(w, "ytr_data_write: the sync to disk failed after %lld rows (errno %d)",
                     (long long)w->rows, errno);
    }
    if (w->io_failed) return YTR_ERR_IO;
    if (last >= 0) w->next_trial = last + 1;
    w->rows++;
    return 0;
}

YTR_API int64_t ytr_data_rows(const ytr_data* w) { return w && w->open ? w->rows : 0; }

YTR_API bool ytr_data_close(ytr_data* w) {
    bool ok;
    if (!w) return false;
    if (!w->open) return ytr__dfail(w, "ytr_data_close: the writer is not open");
    ytr__dspill(w);
    ok = !w->io_failed;
    if (w->file) {
        if (fclose((FILE*)w->file) != 0 && ok) {
            ok = false;
            ytr__dfail(w, "ytr_data_close: closing the file failed (errno %d)", errno);
        }
        w->file = NULL;
    }
    if (w->meta_path[0]) {
        /* The sidecar's last line says the session closed: one without it
         * was cut short. */
        FILE* m = ytr__fopen(w->meta_path, true);
        bool mok = m && fprintf(m, "rows=%lld closed=1\n", (long long)w->rows) > 0;
        if (m) mok = fclose(m) == 0 && mok;
        if (!mok && ok) {
            ok = false;
            ytr__dfail(w, "ytr_data_close: cannot append to '%s'", w->meta_path);
        }
    }
    w->open = false;
    return ok;
}

#endif /* YSP_TRIALS_IMPLEMENTATION_GUARD */
#endif /* YSP_TRIALS_IMPLEMENTATION */

/* ------------------------------------------------------------------------
 * This software is available under the MIT-0 (MIT No Attribution) license.
 *
 * Copyright (c) 2026 ysp contributors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY.
 * ------------------------------------------------------------------------ */
