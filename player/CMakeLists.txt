# The player (docs/player.md): a library so its tests and fuzz target reach
# the same code, and the ysp_player program. Added by the top-level
# CMakeLists.txt when YSP_BUILD_PLAYER is ON.
add_library(ysp_player_lib STATIC impl.c eval.c host.c lower.c run.c)
target_include_directories(ysp_player_lib PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(ysp_player_lib PUBLIC ysp::rig ysp::methods ysp::devices)
target_compile_options(ysp_player_lib PRIVATE ${YSP_WARNINGS})
set_target_properties(ysp_player_lib PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

ysp_add_program(ysp_player ${CMAKE_CURRENT_SOURCE_DIR}/main.c rig methods devices)
target_link_libraries(ysp_player PRIVATE ysp_player_lib)

if(YSP_BUILD_TESTS)
    ysp_add_program(player_make_rig ${PROJECT_SOURCE_DIR}/tests/player/make_rig.c rig devices)
    target_link_libraries(player_make_rig PRIVATE ysp_player_lib)
    ysp_add_program(test_player_eval ${PROJECT_SOURCE_DIR}/tests/player/eval_test.c rig methods devices)
    target_link_libraries(test_player_eval PRIVATE ysp_player_lib)
    add_test(NAME test_player_eval COMMAND test_player_eval)
    # The system tests and the comparison with the C programs build packs
    # and run the program: they need Python, ypak and the examples.
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND AND YSP_BUILD_PACK)
        add_test(NAME test_player_shapes COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/player/player_test.py --bin ${CMAKE_BINARY_DIR}
                                                 --out ${CMAKE_BINARY_DIR}/player_test_shapes)
        add_test(NAME test_player_compare COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/tests/player/compare.py --bin ${CMAKE_BINARY_DIR}
                                                 --out ${CMAKE_BINARY_DIR}/player_test_compare)
    endif()
endif()
