# The pack tool's text layout (docs/layout.md) and the pack tool
# (docs/pack.md). Added by the top-level CMakeLists.txt when YSP_BUILD_LAYOUT
# or YSP_BUILD_PACK is ON; the layout parts need YSP_BUILD_LAYOUT (and
# third_party/, added before this).

if(YSP_BUILD_LAYOUT)
    add_library(ysp_layout STATIC layout.c ysp/layout.h)
    target_include_directories(ysp_layout PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(ysp_layout PUBLIC ysp::rig ysp_skribidi)
    target_compile_options(ysp_layout PRIVATE ${YSP_WARNINGS})
    set_target_properties(ysp_layout PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(ysp_layout PRIVATE YLAY_PATCHES="${YSP_SKRIBIDI_PATCHES}")
    if(UNIX)
        target_link_libraries(ysp_layout PUBLIC m)
    endif()

    if(YSP_BUILD_TESTS)
        # ysp/gfx.h is compiled without SDL for ygfx_cset_check(), as in
        # tests/adapt/outline_test.c. Without the corpus fonts it says that
        # it skipped them and still runs its order checks on a font it writes.
        ysp_add_program(test_layout ${PROJECT_SOURCE_DIR}/tests/layout/layout_test.c rig)
        target_link_libraries(test_layout PRIVATE ysp_layout)
        set(YSP_LAYOUT_TEST_FONTS "C:/Windows/Fonts" CACHE PATH "Font folder of the corpus check (YSP_FONTS_DIR overrides it at run time)")
        target_compile_definitions(test_layout PRIVATE YSCR_NO_SDL
            YLAY_TEST_DIR="${PROJECT_SOURCE_DIR}/tests/layout" YLAY_TEST_FONTS="${YSP_LAYOUT_TEST_FONTS}")
        add_test(NAME test_layout COMMAND test_layout)
        # The costs of docs/layout.md; not a test (run it under the lock).
        ysp_add_program(layout_bench ${PROJECT_SOURCE_DIR}/tests/layout/layout_bench.c rig)
        target_link_libraries(layout_bench PRIVATE ysp_layout)
    endif()

    if(YSP_BUILD_EXAMPLES AND TARGET SDL3::SDL3)
        ysp_add_program(gfx_layout ${PROJECT_SOURCE_DIR}/examples/layout/gfx_layout.c rig)
        target_link_libraries(gfx_layout PRIVATE ysp_layout)
        # PsychoPy's Stroop demos (docs/rig_spec.md 15.5): words laid out here.
        ysp_add_program(trial_stroop ${PROJECT_SOURCE_DIR}/examples/response/trial_stroop.c rig methods)
        target_link_libraries(trial_stroop PRIVATE ysp_layout)
        # jsPsych's instructions and PsychoPy's text demos (docs/rig_spec.md
        # 15.5): pages laid out once and rendered into targets at setup.
        ysp_add_program(trial_instructions ${PROJECT_SOURCE_DIR}/examples/layout/trial_instructions.c rig methods)
        target_link_libraries(trial_instructions PRIVATE ysp_layout)
    endif()
endif()

# The pack tool (docs/pack.md): the library, the ypak CLI and its test. The
# top-level CMakeLists.txt checks that third_party/lodepng was fetched.
if(YSP_BUILD_PACK)
    # lodepng.cpp is C code with a C++ name: compiled as C from a copy, so the
    # tool links its C API without a C++ wrapper. Upstream's warnings off.
    set(_lp ${PROJECT_SOURCE_DIR}/third_party/lodepng)
    configure_file(${_lp}/lodepng.cpp ${CMAKE_CURRENT_BINARY_DIR}/lodepng.c COPYONLY)
    add_library(ysp_lodepng STATIC ${CMAKE_CURRENT_BINARY_DIR}/lodepng.c)
    target_include_directories(ysp_lodepng SYSTEM PUBLIC ${_lp})
    target_compile_definitions(ysp_lodepng PUBLIC LODEPNG_NO_COMPILE_CPP LODEPNG_NO_COMPILE_ENCODER LODEPNG_NO_COMPILE_DISK)
    if(MSVC)
        target_compile_options(ysp_lodepng PRIVATE /W0)
    else()
        target_compile_options(ysp_lodepng PRIVATE -w)
    endif()
    # The pin goes into every manifest; a tree that differs from it says so,
    # as the layout library does for Skribidi's patches.
    file(SHA256 ${_lp}/lodepng.cpp _lp_hash)
    set(_lp_pin "e584a8490db1b76b81bf298c11c09d3b55d9c270")
    if(NOT _lp_hash STREQUAL "6aec876971ec860527859458d3095220fc5ee1258ec37eb59e1e7dd9cf136a81")
        set(_lp_pin "${_lp_pin},tree-differs")
    endif()

    # stb_image.h (JPEG only) and pl_mpeg.h (MPEG-1 indexes), fetched and
    # checked by tools/vendor_pack.py like lodepng. stb_image in its own
    # library so upstream's warnings stay off.
    set(_stb ${PROJECT_SOURCE_DIR}/third_party/stb)
    set(_plm ${PROJECT_SOURCE_DIR}/third_party/pl_mpeg)
    add_library(ysp_stb_image STATIC stb_image.c)
    target_include_directories(ysp_stb_image PRIVATE ${_stb})
    if(MSVC)
        target_compile_options(ysp_stb_image PRIVATE /W0)
    else()
        target_compile_options(ysp_stb_image PRIVATE -w)
    endif()
    file(SHA256 ${_stb}/stb_image.h _stb_hash)
    set(_stb_pin "2.30 2c980bb59875b0d32144a71867fbdebb2f77cd20")
    if(NOT _stb_hash STREQUAL "594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3")
        set(_stb_pin "${_stb_pin},tree-differs")
    endif()
    file(SHA256 ${_plm}/pl_mpeg.h _plm_hash)
    set(_plm_pin "c871f2be022ece7ef4f64230b4fb8e1fb9eb6023")
    if(NOT _plm_hash STREQUAL "3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9")
        set(_plm_pin "${_plm_pin},tree-differs")
    endif()

    # libsamplerate 0.2.2, the best sinc converter only (the opt-in audio
    # resampling, docs/pack.md 4.7). No FMA contraction, so every compiler
    # gives the same samples (docs/pack.md 7).
    set(_lsr ${PROJECT_SOURCE_DIR}/third_party/libsamplerate)
    add_library(ysp_samplerate STATIC ${_lsr}/samplerate.c ${_lsr}/src_sinc.c ${_lsr}/src_linear.c ${_lsr}/src_zoh.c)
    target_include_directories(ysp_samplerate SYSTEM PUBLIC ${_lsr})
    target_compile_definitions(ysp_samplerate PRIVATE ENABLE_SINC_BEST_CONVERTER=1 HAVE_STDBOOL_H=1 PACKAGE="libsamplerate" VERSION="0.2.2")
    if(MSVC)
        target_compile_options(ysp_samplerate PRIVATE /W0 /fp:precise)
    else()
        target_compile_options(ysp_samplerate PRIVATE -w -ffp-contract=off)
        target_link_libraries(ysp_samplerate PUBLIC m)
    endif()
    set(_lsr_pin "0.2.2 c96f5e3de9c4488f4e6c97f59f5245f22fda22f7")
    file(SHA256 ${_lsr}/src_sinc.c _lsr_hash)
    if(NOT _lsr_hash STREQUAL "8e59137d758dae5027410af7f81010e7a9c33892070091866510f0f8b638237e")
        set(_lsr_pin "${_lsr_pin},tree-differs")
    endif()

    add_library(ysp_pack_tool STATIC pack_tool.c pack_impl.c import.c ysp/pack_tool.h)
    target_include_directories(ysp_pack_tool PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    target_include_directories(ysp_pack_tool SYSTEM PRIVATE ${_plm})
    target_link_libraries(ysp_pack_tool PRIVATE ysp_stb_image ysp_samplerate)
    target_compile_definitions(ysp_pack_tool PRIVATE YPT_STB_IMAGE="${_stb_pin}" YPT_PL_MPEG="${_plm_pin}" YPT_LIBSAMPLERATE="${_lsr_pin}")
    # The headers' implementations need ysp::core's threads and libm, and dl
    # for ysp/gfx.h's headless presenter; not ysp::rig, which would link SDL3
    # into a tool that never opens a window.
    target_link_libraries(ysp_pack_tool PUBLIC ysp_lodepng ysp::core ${CMAKE_DL_LIBS})
    if(YSP_BUILD_LAYOUT)
        target_link_libraries(ysp_pack_tool PUBLIC ysp_layout)
    else()
        # PUBLIC: ypak and the tool test see which kinds this build has.
        target_compile_definitions(ysp_pack_tool PUBLIC YPT_NO_LAYOUT)
    endif()
    target_compile_options(ysp_pack_tool PRIVATE ${YSP_WARNINGS})
    target_compile_definitions(ysp_pack_tool PRIVATE YPT_LODEPNG="${_lp_pin}")
    set_target_properties(ysp_pack_tool PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

    add_executable(ypak ypak.c)
    target_link_libraries(ypak PRIVATE ysp_pack_tool)
    target_compile_options(ypak PRIVATE ${YSP_WARNINGS})
    set_target_properties(ypak PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

    if(YSP_BUILD_TESTS)
        add_executable(test_pack_tool ${PROJECT_SOURCE_DIR}/tests/pack/tool_test.c)
        target_link_libraries(test_pack_tool PRIVATE ysp_pack_tool)
        target_compile_options(test_pack_tool PRIVATE ${YSP_WARNINGS})
        set_target_properties(test_pack_tool PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        set(YSP_PACK_TEST_FONTS "C:/Windows/Fonts" CACHE PATH "Font folder of the tool test's system-font cases (YSP_FONTS_DIR overrides it)")
        target_compile_definitions(test_pack_tool PRIVATE YPT_TEST_DIR="${PROJECT_SOURCE_DIR}/tests/pack"
            YPT_TEST_FONTS="${YSP_PACK_TEST_FONTS}")
        add_test(NAME test_pack_tool COMMAND test_pack_tool WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
        # The readers of other formats (pack/import.c) on their own: the test
        # compiles that file into itself, so the mutation runner can mutate it
        # (tests/mutate/import.toml).
        add_executable(test_pack_import ${PROJECT_SOURCE_DIR}/tests/adapt/import_test.c)
        target_include_directories(test_pack_import PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_pack_import PRIVATE ysp_lodepng ysp_stb_image ysp::core)
        target_compile_options(test_pack_import PRIVATE ${YSP_WARNINGS})
        set_target_properties(test_pack_import PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        add_test(NAME test_pack_import COMMAND test_pack_import)
    endif()

    # trial_images (docs/rig_spec.md 15.5), the pack path end to end. Its
    # pack is built here: trial_images_assets writes PNGs made in code and
    # copies the committed conditions CSV, then ypak builds the pack, so the
    # repository holds no binary image and every platform makes the same
    # pack.
    if(YSP_BUILD_EXAMPLES AND TARGET SDL3::SDL3)
        add_executable(trial_images_assets ${PROJECT_SOURCE_DIR}/examples/pack/trial_images_assets.c)
        target_compile_options(trial_images_assets PRIVATE ${YSP_WARNINGS})
        set_target_properties(trial_images_assets PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        set(_ti_src ${CMAKE_CURRENT_BINARY_DIR}/trial_images_src)
        set(_ti_pack ${CMAKE_CURRENT_BINARY_DIR}/trial_images.ysppak)
        add_custom_command(OUTPUT ${_ti_pack}
            COMMAND ${CMAKE_COMMAND} -E make_directory ${_ti_src}/img
            COMMAND trial_images_assets ${_ti_src} ${PROJECT_SOURCE_DIR}/examples/pack/trial_images.csv
            COMMAND ypak build ${_ti_src}/trial_images.json -o ${_ti_pack}
            DEPENDS trial_images_assets ypak ${PROJECT_SOURCE_DIR}/examples/pack/trial_images.csv
            COMMENT "trial_images: its pack, from generated PNGs (ypak build)" VERBATIM)
        add_custom_target(trial_images_pack DEPENDS ${_ti_pack})
        ysp_add_program(trial_images ${PROJECT_SOURCE_DIR}/examples/pack/trial_images.c rig methods)
        add_dependencies(trial_images trial_images_pack)
        target_compile_definitions(trial_images PRIVATE TRIAL_IMAGES_PACK="${_ti_pack}")
    endif()
endif()
