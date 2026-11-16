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
execute_process(
    COMMAND ${CMAKE_COMMAND} -E env SIG_ORACLE_CASES=${CASES} SIG_ORACLE_SEED=${SEED} ${ENV} ${PROG}
    OUTPUT_VARIABLE out RESULT_VARIABLE rc)
string(REGEX MATCH "cases [0-9]+, wrong [0-9]+[^\n]*" summary "${out}")
string(REGEX MATCH "header arm_unverified:[^\n]*" unv "${out}")
message(STATUS "${summary}")
message(STATUS "${unv}")
if(NOT unv MATCHES "B-cannot ([0-9]+) of ([0-9]+)")
    message(FATAL_ERROR "sig_arm_ref: no arm_unverified line (exit ${rc})")
endif()
set(counted ${CMAKE_MATCH_1})
set(cannot ${CMAKE_MATCH_2})
if(NOT unv MATCHES "([0-9]+) with none")
    message(FATAL_ERROR "sig_arm_ref: no unflagged-wrong count")
endif()
set(bare ${CMAKE_MATCH_1})
if(NOT counted EQUAL cannot)
    message(FATAL_ERROR "sig_arm_ref: ${counted} of ${cannot} cases B cannot run are counted")
endif()
if(NOT bare EQUAL 0)
    message(FATAL_ERROR "sig_arm_ref: ${bare} wrong cases with no count")
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
