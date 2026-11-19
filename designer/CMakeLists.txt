# The designer (docs/designer.md). Added by the top-level CMakeLists.txt when
# YSP_BUILD_DESIGNER is ON, which needs the player, the pack tool and the
# layout library.
#
# ysp_designer_core: the parts with no window (designer/core/dsg.h), tested
# headless by test_designer_core. ysp_designer: the window (designer/app),
# built when SDL3, GL ES and Dear ImGui (tools/vendor_designer.py) are there.
# test_designer_te: only with YSP_DESIGNER_TEST_ENGINE (off by default).

# On Windows the tests import SDL3 through the player's header
# implementations, and only ysp_add_program() puts the DLL beside a program.
function(dsg_sdl_dll t)
    if(WIN32 AND TARGET SDL3::SDL3-shared)
        add_custom_command(TARGET ${t} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${t}>)
    endif()
endfunction()

# designer_tests builds every designer test, so CI names one target and a
# new test needs no change there (a test CI did not build showed as "Not
# Run"). Each test registers itself with dsg_test().
add_custom_target(designer_tests)
function(dsg_test t)
    dsg_sdl_dll(${t})
    add_dependencies(designer_tests ${t})
endfunction()

# The player and the pack tool with one set of header implementations: the
# player's (ysp_player_impl, with SDL and miniaudio), and the pack tool's
# code without its own copies of the shared ones (ysp_pack_tool_code, not
# ysp_pack_tool). pack/CMakeLists.txt says how the libraries are split.
# Step 4 (docs/designer.md, "Plan of step 4"): the document (doc.c, shared),
# the chart (chart.c, part A), the tables (csv.c, part B) and the panels'
# fields (schema.c, part C).
add_library(ysp_designer_core STATIC
    core/dsg.h core/check.c core/locate.c core/platform.c core/project.c core/redraw.c
    core/doc.h core/doc.c core/chart.h core/chart.c core/csv.h core/csv.c core/schema.h core/schema.c)
target_include_directories(ysp_designer_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/core)
target_compile_definitions(ysp_designer_core PUBLIC $<$<C_COMPILER_ID:MSVC>:_CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_DEPRECATE>)
target_link_libraries(ysp_designer_core PUBLIC ysp_player_lib ysp_pack_tool_code ysp_player_impl)
target_compile_options(ysp_designer_core PRIVATE ${YSP_WARNINGS})
set_target_properties(ysp_designer_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
ysp_profiled(ysp_designer_core)

if(YSP_BUILD_TESTS)
    # Projects from tests/player/, copied under the build tree; stroop also
    # needs a font (YSP_DESIGNER_TEST_FONT; skipped without one).
    set(YSP_DESIGNER_TEST_FONT "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf" CACHE FILEPATH
        "A TrueType font for the designer test's stroop project (skipped when missing)")
    add_executable(test_designer_core ${PROJECT_SOURCE_DIR}/tests/designer/core_test.c)
    target_link_libraries(test_designer_core PRIVATE ysp_designer_core)
    target_compile_options(test_designer_core PRIVATE ${YSP_WARNINGS})
    set_target_properties(test_designer_core PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_compile_definitions(test_designer_core PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        YSP_DESIGNER_TEST_FONT="${YSP_DESIGNER_TEST_FONT}")
    add_test(NAME test_designer_core COMMAND test_designer_core ${CMAKE_CURRENT_BINARY_DIR}/designer_test_work)
    dsg_test(test_designer_core)

    # Step 4: the document (shared), then one test per part, each in its
    # part's own file. bench_designer_doc times an edit; it is not a test.
    foreach(_t doc chart table panels)
        add_executable(test_designer_${_t} ${PROJECT_SOURCE_DIR}/tests/designer/${_t}_test.c)
        target_link_libraries(test_designer_${_t} PRIVATE ysp_designer_core)
        target_compile_options(test_designer_${_t} PRIVATE ${YSP_WARNINGS})
        set_target_properties(test_designer_${_t} PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_definitions(test_designer_${_t} PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
        dsg_test(test_designer_${_t})
    endforeach()
    add_test(NAME test_designer_doc COMMAND test_designer_doc ${CMAKE_CURRENT_BINARY_DIR}/designer_doc_work)
    add_test(NAME test_designer_chart COMMAND test_designer_chart ${CMAKE_CURRENT_BINARY_DIR}/designer_chart_work)
    add_test(NAME test_designer_table COMMAND test_designer_table ${CMAKE_CURRENT_BINARY_DIR}/designer_table_work)
    add_test(NAME test_designer_panels COMMAND test_designer_panels ${CMAKE_CURRENT_BINARY_DIR}/designer_panels_work)
    if(NOT WIN32)
        add_executable(bench_designer_doc ${PROJECT_SOURCE_DIR}/tests/designer/doc_bench.c)
        target_link_libraries(bench_designer_doc PRIVATE ysp_designer_core)
        set_target_properties(bench_designer_doc PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
        target_compile_definitions(bench_designer_doc PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}")
    endif()
endif()

# --- the window ---------------------------------------------------------------

set(_imgui ${PROJECT_SOURCE_DIR}/third_party/imgui)
set(_skbx ${PROJECT_SOURCE_DIR}/third_party/skribidi-extra)
if(NOT EXISTS ${_imgui}/imgui.cpp OR NOT EXISTS ${_skbx}/src/skb_image_atlas.c)
    if(YSP_DESIGNER_TEST_ENGINE)
        message(FATAL_ERROR "YSP_DESIGNER_TEST_ENGINE needs Dear ImGui and Skribidi's rasterizer; fetch them with: "
            "uv run --no-project python tools/vendor_designer.py --write --test-engine")
    endif()
    message(STATUS "ysp_designer: Dear ImGui or Skribidi's rasterizer is missing, so only the core builds; "
        "fetch them with: uv run --no-project python tools/vendor_designer.py --write")
    return()
endif()

# Skribidi's rasterizer, image atlas and layout cache, from the layout
# library's pinned commit: third_party/skribidi has only what the pack tool
# compiles.
add_library(ysp_designer_skb STATIC ${_skbx}/src/skb_rasterizer.c ${_skbx}/src/skb_image_atlas.c
    ${_skbx}/src/skb_canvas.c ${_skbx}/src/skb_layout_cache.c)
target_include_directories(ysp_designer_skb SYSTEM PUBLIC ${_skbx}/include)
target_include_directories(ysp_designer_skb PRIVATE ${PROJECT_SOURCE_DIR}/third_party/skribidi/src)
target_link_libraries(ysp_designer_skb PUBLIC ysp_skribidi)
if(MSVC)
    target_compile_options(ysp_designer_skb PRIVATE /W0)
else()
    target_compile_options(ysp_designer_skb PRIVATE -w)
endif()
set_target_properties(ysp_designer_skb PROPERTIES C_STANDARD 17 C_STANDARD_REQUIRED ON C_EXTENSIONS ON)

# Dear ImGui and the UI layer without SDL or GL (app/ui.cpp), so the test can
# drive it headless. C++20 for Skribidi's designated initializers; no
# exceptions, no RTTI, no thread-safe statics: the program needs a C++
# compiler and no C++ runtime (docs/imgui_probe.md, finding 6).
set(_ui_sources app/ui.cpp app/ui.h app/skt.cpp app/skt.h app/skf.cpp app/skf.h app/imconfig_designer.h
    app/views.h app/views.cpp app/view_chart.cpp app/view_tables.h app/view_tables.cpp app/view_panels.cpp
    ${_imgui}/imgui.cpp ${_imgui}/imgui_draw.cpp ${_imgui}/imgui_tables.cpp ${_imgui}/imgui_widgets.cpp)
add_library(ysp_designer_ui STATIC ${_ui_sources})
target_include_directories(ysp_designer_ui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/app)
target_include_directories(ysp_designer_ui SYSTEM PUBLIC ${_imgui})
target_compile_definitions(ysp_designer_ui PUBLIC IMGUI_USER_CONFIG="imconfig_designer.h")
target_link_libraries(ysp_designer_ui PUBLIC ysp_designer_core ysp_designer_skb ysp_skribidi)
set_target_properties(ysp_designer_ui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
if(MSVC)
    target_compile_options(ysp_designer_ui PUBLIC $<$<COMPILE_LANGUAGE:CXX>:/GR- /EHs-c->)
    target_compile_definitions(ysp_designer_ui PUBLIC _HAS_EXCEPTIONS=0)
else()
    target_compile_options(ysp_designer_ui PUBLIC $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
endif()

# Fonts for user text (Skribidi) and the UI's own text, copied beside the
# program; none is committed. The order is the fallback order.
set(YSP_DESIGNER_FONTS
    "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf;/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf;/usr/share/fonts/truetype/noto/NotoSansArabic-Regular.ttf;/usr/share/fonts/truetype/noto/NotoSansHebrew-Regular.ttf;/usr/share/fonts/truetype/noto/NotoSansDevanagari-Regular.ttf;/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf;/usr/share/fonts/truetype/noto/NotoSansSymbols-Regular.ttf;/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
    CACHE STRING "Font files for the designer's text, in fallback order (missing ones are skipped)")
set(_fontlist "")
foreach(f IN LISTS YSP_DESIGNER_FONTS)
    if(EXISTS ${f})
        get_filename_component(_fn ${f} NAME)
        configure_file(${f} ${CMAKE_CURRENT_BINARY_DIR}/fonts/${_fn} COPYONLY)
        string(APPEND _fontlist "${_fn}\n")
    endif()
endforeach()
file(WRITE ${CMAKE_CURRENT_BINARY_DIR}/fonts/fonts.txt "${_fontlist}")

if(YSP_BUILD_TESTS)
    # The UI with no window and no GL: ImGui's frames driven with made-up
    # input, its textures accepted without a renderer.
    add_executable(test_designer_ui ${PROJECT_SOURCE_DIR}/tests/designer/ui_test.cpp)
    target_link_libraries(test_designer_ui PRIVATE ysp_designer_ui)
    target_compile_definitions(test_designer_ui PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        YSP_DESIGNER_FONT_DIR="${CMAKE_CURRENT_BINARY_DIR}/fonts")
    set_target_properties(test_designer_ui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME test_designer_ui COMMAND test_designer_ui ${CMAKE_CURRENT_BINARY_DIR}/designer_ui_work)
    dsg_test(test_designer_ui)

    # The Tables view with 10,000 rows: only the visible cells are laid out
    # (docs/designer.md, "The tables"); --bench times a scroll.
    add_executable(test_designer_table_ui ${PROJECT_SOURCE_DIR}/tests/designer/table_ui_test.cpp)
    target_link_libraries(test_designer_table_ui PRIVATE ysp_designer_ui)
    target_compile_definitions(test_designer_table_ui PRIVATE YSP_DESIGNER_FONT_DIR="${CMAKE_CURRENT_BINARY_DIR}/fonts")
    set_target_properties(test_designer_table_ui PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    add_test(NAME test_designer_table_ui COMMAND test_designer_table_ui ${CMAKE_CURRENT_BINARY_DIR}/designer_table_ui_work)
    dsg_test(test_designer_table_ui)

    # Times the chart's frames (docs/designer.md, "The chart"); not a test.
    if(NOT WIN32)
        add_executable(bench_designer_chart ${PROJECT_SOURCE_DIR}/tests/designer/chart_bench.cpp)
        target_link_libraries(bench_designer_chart PRIVATE ysp_designer_ui)
        target_compile_definitions(bench_designer_chart PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}" YSP_DESIGNER_FONT_DIR="${CMAKE_CURRENT_BINARY_DIR}/fonts")
        set_target_properties(bench_designer_chart PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    endif()
endif()

if(YSP_BUILD_TESTS AND YSP_DESIGNER_TEST_ENGINE)
    # The UI driven by the Dear ImGui Test Engine (docs/designer.md, "UI tests
    # with the Test Engine"). Its hooks need IMGUI_ENABLE_TEST_ENGINE in every
    # file that includes imgui.h, so the UI layer and ImGui compile again here
    # with tests/designer/imconfig_designer_te.h; ysp_designer_ui and the
    # program keep their own objects and flags.
    set(_te ${PROJECT_SOURCE_DIR}/third_party/imgui_test_engine)
    if(NOT EXISTS ${_te}/imgui_te_engine.cpp)
        message(FATAL_ERROR "YSP_DESIGNER_TEST_ENGINE: the Test Engine is not in third_party/imgui_test_engine. Read its "
            "license in docs/designer.md, then fetch it with: uv run --no-project python tools/vendor_designer.py --write --test-engine")
    endif()
    set(_te_config ${PROJECT_SOURCE_DIR}/tests/designer)

    # The engine: no -fno-threadsafe-statics, because its coroutines are
    # threads (std::thread) and it has function-local statics.
    add_library(ysp_designer_te_engine STATIC
        ${_te}/imgui_te_engine.cpp ${_te}/imgui_te_context.cpp ${_te}/imgui_te_coroutine.cpp ${_te}/imgui_te_ui.cpp
        ${_te}/imgui_te_utils.cpp ${_te}/imgui_te_exporters.cpp ${_te}/imgui_te_perftool.cpp ${_te}/imgui_capture_tool.cpp)
    target_include_directories(ysp_designer_te_engine SYSTEM PUBLIC ${_te} ${_imgui})
    target_include_directories(ysp_designer_te_engine PUBLIC ${_te_config} ${CMAKE_CURRENT_SOURCE_DIR}/app)
    target_compile_definitions(ysp_designer_te_engine PUBLIC IMGUI_USER_CONFIG="imconfig_designer_te.h")
    target_link_libraries(ysp_designer_te_engine PUBLIC Threads::Threads)
    set_target_properties(ysp_designer_te_engine PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(ysp_designer_te_engine PRIVATE /GR- /EHs-c- /W0)
        target_compile_definitions(ysp_designer_te_engine PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(ysp_designer_te_engine PRIVATE -fno-exceptions -fno-rtti -w)
    endif()

    # The runner, the UI layer and ImGui, with the designer's flags.
    add_executable(test_designer_te ${PROJECT_SOURCE_DIR}/tests/designer/te_test.cpp ${_te_config}/imconfig_designer_te.h
        ${_te_config}/te_common.h ${_te_config}/te_chart.cpp ${_te_config}/te_tables.cpp ${_te_config}/te_panels.cpp
        ${_ui_sources})
    target_link_libraries(test_designer_te PRIVATE ysp_designer_te_engine ysp_designer_core ysp_designer_skb ysp_skribidi)
    target_compile_definitions(test_designer_te PRIVATE YSP_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
        YSP_DESIGNER_FONT_DIR="${CMAKE_CURRENT_BINARY_DIR}/fonts")
    set_target_properties(test_designer_te PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    if(MSVC)
        target_compile_options(test_designer_te PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/GR- /EHs-c->)
        target_compile_definitions(test_designer_te PRIVATE _HAS_EXCEPTIONS=0)
    else()
        target_compile_options(test_designer_te PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions -fno-rtti -fno-threadsafe-statics>)
    endif()
    add_test(NAME test_designer_te COMMAND test_designer_te ${CMAKE_CURRENT_BINARY_DIR}/designer_te_work)
    set_tests_properties(test_designer_te PROPERTIES TIMEOUT 120)
    dsg_test(test_designer_te)
endif()

if(NOT TARGET SDL3::SDL3)
    message(STATUS "ysp_designer: no SDL3, so the program is not built (the core and UI tests are)")
    return()
endif()
if(NOT EMSCRIPTEN)
    find_library(YSP_GLESV2 GLESv2)
    if(NOT YSP_GLESV2)
        message(STATUS "ysp_designer: no libGLESv2, so the program is not built")
        return()
    endif()
endif()

add_executable(ysp_designer app/main.cpp ${_imgui}/backends/imgui_impl_sdl3.cpp ${_imgui}/backends/imgui_impl_opengl3.cpp)
target_include_directories(ysp_designer PRIVATE ${_imgui}/backends)
target_compile_definitions(ysp_designer PRIVATE IMGUI_IMPL_OPENGL_ES3)
target_link_libraries(ysp_designer PRIVATE ysp_designer_ui SDL3::SDL3 ${YSP_GLESV2})
# The designer's own C++ and ImGui need no C++ runtime; the program still
# links libstdc++ for one symbol of the vendored HarfBuzz
# (std::__throw_system_error), as ypak does (docs/designer.md).
set_target_properties(ysp_designer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
