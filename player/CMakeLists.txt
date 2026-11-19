# The player (docs/player.md): a library so its tests and fuzz target reach
# the same code, and the ysp_player program. Added by the top-level
# CMakeLists.txt when YSP_BUILD_PLAYER is ON.
#
# ysp_player_impl: the header implementations (impl.c), with SDL and
# miniaudio. Its own target so that a program with the player and the pack
# tool (the designer) links one set of implementations: this one, with
# ysp_pack_tool_code, and not ysp_pack_tool's (pack/CMakeLists.txt).
# ysp_player_lib links it, so the player's other consumers see no change.
add_library(ysp_player_impl STATIC impl.c)
add_library(ysp_player_lib STATIC eval.c host.c lower.c run.c v2.c)
target_link_libraries(ysp_player_lib PUBLIC ysp_player_impl)
foreach(t ysp_player_impl ysp_player_lib)
    target_include_directories(${t} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(${t} PUBLIC ysp::rig ysp::methods ysp::devices)
    target_compile_options(${t} PRIVATE ${YSP_WARNINGS})
    # fopen(), getenv() and strcpy() are the C library's; MSVC's /W4 calls them
    # unsafe and asks for its own (C4996)
    target_compile_definitions(${t} PUBLIC $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE>)
    set_target_properties(${t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    ysp_profiled(${t})
endforeach()

ysp_add_program(ysp_player ${CMAKE_CURRENT_SOURCE_DIR}/main.c rig methods devices)
target_link_libraries(ysp_player PRIVATE ysp_player_lib)

if(YSP_BUILD_TESTS)
    ysp_add_program(player_make_rig ${PROJECT_SOURCE_DIR}/tests/player/make_rig.c rig devices)
    target_link_libraries(player_make_rig PRIVATE ysp_player_lib)
    ysp_add_program(test_player_eval ${PROJECT_SOURCE_DIR}/tests/player/eval_test.c rig methods devices)
    target_link_libraries(test_player_eval PRIVATE ysp_player_lib)
    add_test(NAME test_player_eval COMMAND test_player_eval)
    # GNU ld's --wrap: neither MSVC's linker nor Apple's has it
    if(NOT WIN32 AND NOT APPLE)
        ysp_add_program(player_alloc_test ${PROJECT_SOURCE_DIR}/tests/player/alloc_test.c rig methods devices)
        target_link_libraries(player_alloc_test PRIVATE ysp_player_lib -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc)
    endif()
    # pl_check() (docs/player.md); with --wrap it also counts the heap blocks
    # that stay live over repeated calls
    ysp_add_program(test_player_check ${PROJECT_SOURCE_DIR}/tests/player/check_test.c rig methods devices)
    target_link_libraries(test_player_check PRIVATE ysp_player_lib)
    if(NOT WIN32 AND NOT APPLE)
        target_compile_definitions(test_player_check PRIVATE PL_CHECK_COUNT)
        target_link_libraries(test_player_check PRIVATE -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free)
    endif()
    ysp_add_program(player_fuzz ${PROJECT_SOURCE_DIR}/tests/fuzz/player_fuzz.c rig methods devices)
    target_link_libraries(player_fuzz PRIVATE ysp_player_lib)
    # The system tests and the comparison with the C programs build packs
    # and run the program: they need Python, ypak and the examples.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND YSP_BUILD_PACK)
        add_test(NAME test_player_shapes COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/player/player_test.py --bin ${CMAKE_BINARY_DIR}
                                                 --out ${CMAKE_BINARY_DIR}/player_test_shapes)
        add_test(NAME test_player_check COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/player/check_test.py --bin ${CMAKE_BINARY_DIR}
                                                --out ${CMAKE_BINARY_DIR}/player_test_check)
        add_test(NAME test_player_compare COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/player/compare.py --bin ${CMAKE_BINARY_DIR}
                                                 --out ${CMAKE_BINARY_DIR}/player_test_compare)
    endif()
endif()
