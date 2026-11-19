# The designer (docs/designer.md). Added by the top-level CMakeLists.txt when
# YSP_BUILD_DESIGNER is ON, which needs the player, the pack tool and the
# layout library.
#
# ysp_designer_core: the parts with no window (designer/core/dsg.h), tested
# headless by test_designer_core. ysp_designer: the window (designer/app),
# built when SDL3, GL ES and Dear ImGui (tools/vendor_designer.py) are there.

# The player and the pack tool with one set of header implementations: the
# player's (ysp_player_impl, with SDL and miniaudio), and the pack tool's
# code without its own copies of the shared ones (ysp_pack_tool_code, not
# ysp_pack_tool). pack/CMakeLists.txt says how the libraries are split.
add_library(ysp_designer_core STATIC
    core/dsg.h core/check.c core/locate.c core/platform.c core/project.c core/redraw.c)
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
endif()

# --- the window ---------------------------------------------------------------

set(_imgui ${PROJECT_SOURCE_DIR}/third_party/imgui)
set(_skbx ${PROJECT_SOURCE_DIR}/third_party/skribidi-extra)
if(NOT EXISTS ${_imgui}/imgui.cpp OR NOT EXISTS ${_skbx}/src/skb_image_atlas.c)
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
add_library(ysp_designer_ui STATIC
    app/ui.cpp app/ui.h app/skt.cpp app/skt.h app/skf.cpp app/skf.h app/imconfig_designer.h
    ${_imgui}/imgui.cpp ${_imgui}/imgui_draw.cpp ${_imgui}/imgui_tables.cpp ${_imgui}/imgui_widgets.cpp)
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
