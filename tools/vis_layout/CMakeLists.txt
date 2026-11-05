# The text layout of ysp-vis (docs/bindings.md, ysp-vis): pack/layout.c and
# the vendored Skribidi, HarfBuzz, SheenBidi, libunibreak and budouxc of
# third_party/, as static libraries that the Python extension and the MEX
# core link. tools/vendor_vis.py --layout configures, builds and installs
# this project once per toolchain (MSVC /MD for the wheel and MATLAB,
# Octave's own MinGW gcc for Octave), as it builds SDL3.
#
# layout.c compiles with YSCR_NO_SDL: it uses ysp/gfx.h's and
# ysp/outline.h's types only and calls nothing in either (pack/ysp/layout.h);
# the core that links it compiles ysp/outline.h's implementation.
cmake_minimum_required(VERSION 3.21)
project(ysp_vis_layout C CXX)

set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreadedDLL)
get_filename_component(YSP_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../.. ABSOLUTE)
if(NOT EXISTS ${YSP_ROOT}/third_party/harfbuzz/src/harfbuzz.cc)
    message(FATAL_ERROR "third_party/ has no layout sources: uv run --no-project python tools/vendor_layout.py --write")
endif()

add_subdirectory(${YSP_ROOT}/third_party third_party)

add_library(ysp_vis_layout STATIC ${YSP_ROOT}/pack/layout.c)
target_include_directories(ysp_vis_layout PRIVATE ${YSP_ROOT}/include ${YSP_ROOT}/pack)
target_compile_definitions(ysp_vis_layout PRIVATE YSCR_NO_SDL YLAY_PATCHES="${YSP_SKRIBIDI_PATCHES}")
target_link_libraries(ysp_vis_layout PRIVATE ysp_skribidi)
set_target_properties(ysp_vis_layout PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)

install(TARGETS ysp_vis_layout ysp_skribidi ysp_harfbuzz ysp_sheenbidi ysp_unibreak ysp_budoux ARCHIVE DESTINATION lib)
install(FILES ${YSP_ROOT}/pack/ysp/layout.h DESTINATION include/ysp)
