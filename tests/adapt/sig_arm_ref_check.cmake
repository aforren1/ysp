# ctest: the reviewer's oracle (tests/adapt/sig_arm_ref.c) at a fixed seed.
# The pass criterion is the two targets of docs/sig.md, "Exactness": every
# case the oracle's B cannot run is counted in arm_unverified, and every
# wrong case is counted (arm_unverified, a fallback, a retracted hit or a
# stale row or event). A wrong case inside the bound is reported, not a
# failure. Called as cmake -DPROG=... -DCASES=... -DSEED=... -P this file;
# -DENV="NAME=value;NAME=value" adds the oracle's switches (sig.h 0.7.0:
# late mode 4 with A's checks, once with the late generator off).
# -DFEEDN=1: the oracle's ysig_feed_n() mode (SIG_ORACLE_FEEDN=1 in ENV);
# format 3.11 rule 4 as decided 2026-11-15: no batch falls back where
# per-event feeding does not ("did not fit" 0), and a batched graph differs
# from the per-event one only in a count, and lower.
#
# The final review's M4 (2026-11-16): the oracle must have run CASES
# cases, and at the fixed seed three numbers have upper bounds, so a
# header that counts more than it must fails: -DWRONG_MAX (wrong cases),
# -DFLAGGED_MAX (right cases with a count) and -DCOUNTS_MAX (counts in
# right cases). Each is the run at 2fbd646 (Linux desktop, GCC 12.2,
# Release) plus 1 % rounded up, for another compiler's rounding; the
# numbers and the margin are in CMakeLists.txt. -DBARE_MAX (default 0):
# wrong cases with no count allowed; H1 (docs/sig.md, "Per device") makes
# one at seed 20261115 with the late generator on: case 144, a raw
# mouse's count before the last barrier, which the header keeps and the
# oracle's R drops (the adapter's integrator one count apart).
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env SIG_ORACLE_CASES=${CASES} SIG_ORACLE_SEED=${SEED} ${ENV} ${PROG}
    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
string(REGEX MATCH "cases [0-9]+, wrong [0-9]+[^\n]*" summary "${out}")
string(REGEX MATCH "header arm_unverified:[^\n]*" unv "${out}")
message(STATUS "${summary}")
message(STATUS "${unv}")
if(NOT summary MATCHES "^cases ([0-9]+), wrong ([0-9]+)")
    message(FATAL_ERROR "sig_arm_ref: no summary line (exit ${rc})")
endif()
set(ran ${CMAKE_MATCH_1})
set(wrong ${CMAKE_MATCH_2})
if(NOT ran EQUAL CASES)
    message(FATAL_ERROR "sig_arm_ref: ${ran} cases ran, ${CASES} asked")
endif()
if(NOT unv MATCHES "B-cannot ([0-9]+) of ([0-9]+)")
    message(FATAL_ERROR "sig_arm_ref: no arm_unverified line (exit ${rc})")
endif()
set(counted ${CMAKE_MATCH_1})
set(cannot ${CMAKE_MATCH_2})
if(NOT unv MATCHES "([0-9]+) with none")
    message(FATAL_ERROR "sig_arm_ref: no unflagged-wrong count")
endif()
set(bare ${CMAKE_MATCH_1})
if(NOT unv MATCHES "right ([0-9]+) of ([0-9]+) cases; ([0-9]+) counts over")
    message(FATAL_ERROR "sig_arm_ref: no right-cases count")
endif()
set(flagged ${CMAKE_MATCH_1})
set(counts ${CMAKE_MATCH_3})
if(NOT counted EQUAL cannot)
    message(FATAL_ERROR "sig_arm_ref: ${counted} of ${cannot} cases B cannot run are counted")
endif()
if(NOT DEFINED BARE_MAX)
    set(BARE_MAX 0)
endif()
if(bare GREATER BARE_MAX)
    message(FATAL_ERROR "sig_arm_ref: ${bare} wrong cases with no count (at most ${BARE_MAX})")
endif()
foreach(b WRONG_MAX FLAGGED_MAX COUNTS_MAX)
    if(NOT DEFINED ${b})
        message(FATAL_ERROR "sig_arm_ref: -D${b} not given")
    endif()
endforeach()
if(wrong GREATER WRONG_MAX)
    message(FATAL_ERROR "sig_arm_ref: ${wrong} wrong cases, at most ${WRONG_MAX}")
endif()
if(flagged GREATER FLAGGED_MAX)
    message(FATAL_ERROR "sig_arm_ref: ${flagged} right cases with a count, at most ${FLAGGED_MAX}")
endif()
if(counts GREATER COUNTS_MAX)
    message(FATAL_ERROR "sig_arm_ref: ${counts} counts in right cases, at most ${COUNTS_MAX}")
endif()
if(FEEDN)
    if(NOT out MATCHES "a batch did not fit \\(late_full, declared; not compared on\\): 0\\.")
        message(FATAL_ERROR "sig_arm_ref: a batch did not fit (or no feed_n summary)")
    endif()
    if(NOT out MATCHES "cases where batched differs from per event: ([0-9]+)")
        message(FATAL_ERROR "sig_arm_ref: no feed_n summary")
    endif()
    set(ndiff ${CMAKE_MATCH_1})
    # its lines hold "; ", a list separator here
    string(REPLACE ";" "," fo "${out}")
    string(REGEX MATCHALL "feed_n: case [0-9]+ differs at call [^\n]*" diffs "${fo}")
    list(LENGTH diffs shown)
    if(NOT shown EQUAL ndiff)
        message(FATAL_ERROR "sig_arm_ref: ${ndiff} batched differences, ${shown} shown")
    endif()
    foreach(d IN LISTS diffs)
        if(NOT d MATCHES "count [a-z_]+: per event ([0-9]+), batched ([0-9]+)")
            message(FATAL_ERROR "sig_arm_ref: not a count: ${d}")
        endif()
        if(NOT CMAKE_MATCH_2 LESS CMAKE_MATCH_1)
            message(FATAL_ERROR "sig_arm_ref: batched not lower: ${d}")
        endif()
    endforeach()
    message(STATUS "feed_n: ${ndiff} cases differ, each a count, batched lower")
endif()
