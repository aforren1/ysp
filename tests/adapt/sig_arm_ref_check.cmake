# ctest: the reviewer's oracle (tests/adapt/sig_arm_ref.c) at a fixed seed.
# The pass criterion is the two targets of docs/sig.md, "Exactness": every
# case the oracle's B cannot run is counted in arm_unverified, and every
# wrong case is counted (arm_unverified, a fallback, a retracted hit or a
# stale row or event). A wrong case inside the bound is reported, not a
# failure. Called as cmake -DPROG=... -DCASES=... -DSEED=... -P this file;
# -DENV="NAME=value;NAME=value" adds the oracle's switches (sig.h 0.7.0:
# late mode 4 with A's checks, once with the late generator off).
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
