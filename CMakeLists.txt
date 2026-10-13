cmake_minimum_required(VERSION 3.20)
project(ysp LANGUAGES C CXX)

# Consumers: add_subdirectory(ysp), then link the group whose headers the
# program includes:
#   ysp::core     rt, timeline, table, json, pack
#   ysp::methods  stair, quest, aep, trials
#   ysp::devices  serial, parallel, input, response, box, device, rigfile, net
#   ysp::rig      screen, gfx, color, outline, rdk, video, audio
# or ysp::ysp, all four. Each group carries the include path, the groups its
# headers include, and what an IMPLEMENTATION translation unit needs to link,
# so that a program pays only for the headers it uses: a methods-only program
# does not link setupapi or SDL3.
find_package(Threads REQUIRED)
set(YSP_INCLUDE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/include)

# pthreads for ysp/rt.h's workers on POSIX. libm on Linux, where a C driver
# does not get it for free: the timeline's easings and the table's parsing
# call into it already, so every group gets it from here.
add_library(ysp_core INTERFACE)
add_library(ysp::core ALIAS ysp_core)
target_include_directories(ysp_core INTERFACE ${YSP_INCLUDE_DIR})
target_link_libraries(ysp_core INTERFACE Threads::Threads $<$<PLATFORM_ID:Linux>:m>)

# ysp/quest.h and ysp/aep.h include ysp/rt.h for their opt-in async layer, and
# ysp/trials.h includes ysp/table.h.
add_library(ysp_methods INTERFACE)
add_library(ysp::methods ALIAS ysp_methods)
target_include_directories(ysp_methods INTERFACE ${YSP_INCLUDE_DIR})
target_link_libraries(ysp_methods INTERFACE ysp::core)

# The transports include ysp/rt.h; setupapi is serial-port enumeration; dl
# is ysp/net.h loading liblsl at run time.
add_library(ysp_devices INTERFACE)
add_library(ysp::devices ALIAS ysp_devices)
target_include_directories(ysp_devices INTERFACE ${YSP_INCLUDE_DIR})
target_link_libraries(ysp_devices INTERFACE ysp::core $<$<PLATFORM_ID:Windows>:setupapi> ${CMAKE_DL_LIBS})

# ysp/screen.h includes ysp/input.h. dl because ysp/gfx.h's headless
# presenter loads libEGL with dlopen on POSIX, as miniaudio loads its
# backends. SDL3, miniaudio and pl_mpeg come from ysp_link_rig_deps below.
add_library(ysp_rig INTERFACE)
add_library(ysp::rig ALIAS ysp_rig)
target_include_directories(ysp_rig INTERFACE ${YSP_INCLUDE_DIR})
target_link_libraries(ysp_rig INTERFACE ysp::core ysp::devices ${CMAKE_DL_LIBS})

# The union, so consumers from before the groups keep working.
add_library(ysp INTERFACE)
add_library(ysp::ysp ALIAS ysp)
target_link_libraries(ysp INTERFACE ysp::core ysp::methods ysp::devices ysp::rig)

# The rig's optional dependencies are linked when they are there and skipped
# when not: YSCR_NO_SDL, YAU_NO_MINIAUDIO and YVID_NO_PL_MPEG build the
# headers without them, and only the consumer knows which of those it wants.
# A consumer finds SDL3 and sets YSP_MINIAUDIO_DIR or YSP_PL_MPEG_DIR before
# add_subdirectory(ysp); this project's own build calls it after its search.
function(ysp_link_rig_deps sdl_hint)
    if(TARGET SDL3::SDL3)
        target_link_libraries(ysp_rig INTERFACE SDL3::SDL3)
    else()
        message(STATUS "ysp::rig: SDL3 not found, so it does not link it; ${sdl_hint}")
    endif()
    if(YSP_MINIAUDIO_DIR AND EXISTS ${YSP_MINIAUDIO_DIR}/miniaudio.h)
        add_library(ysp_miniaudio INTERFACE)
        add_library(ysp::miniaudio ALIAS ysp_miniaudio)
        target_include_directories(ysp_miniaudio INTERFACE ${YSP_MINIAUDIO_DIR})
        target_link_libraries(ysp_miniaudio INTERFACE ${CMAKE_DL_LIBS})
        target_link_libraries(ysp_rig INTERFACE ysp::miniaudio)
    endif()
    if(YSP_PL_MPEG_DIR AND EXISTS ${YSP_PL_MPEG_DIR}/pl_mpeg.h)
        add_library(ysp_pl_mpeg INTERFACE)
        add_library(ysp::pl_mpeg ALIAS ysp_pl_mpeg)
        target_include_directories(ysp_pl_mpeg INTERFACE ${YSP_PL_MPEG_DIR})
        target_link_libraries(ysp_rig INTERFACE ysp::pl_mpeg)
    endif()
endfunction()

# Nothing below applies when ysp is pulled in as a subproject.
if(NOT CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    ysp_link_rig_deps("call find_package(SDL3) before add_subdirectory(ysp)")
    return()
endif()

option(YSP_BUILD_EXAMPLES "Build the programs in examples/" ON)
option(YSP_BUILD_TESTS    "Build the compile checks in tests/compile/" ON)
# Off by default and never added to ctest: a loopback test needs two device
# names that are wired together, which CI does not have.
option(YSP_BUILD_LOOPBACK "Build the loopback tests in tests/loopback/ (needs hardware)" OFF)
# Off by default because it downloads Tracy: builds ysp/rt.h's compile check
# with YRT_TRACY against the real TracyC.h, so the macro mapping is checked
# against Tracy itself and not against a copy of it.
option(YSP_TRACY_CHECK "Fetch Tracy and build the YRT_TRACY compile check" OFF)
# Off by default: text layout for the pack tool and the player (pack/, the
# vendored Skribidi and HarfBuzz in third_party/, examples/layout/gfx_layout.c and
# tests/layout/). About 8 MB of sources that no header needs.
option(YSP_BUILD_LAYOUT "Build the layout library (pack/, third_party/), its test and gfx_layout" OFF)
# Off by default: the pack tool (pack/pack_tool.c, the ypak CLI, tests/pack/).
# It needs lodepng in third_party/lodepng/ (tools/vendor_pack.py fetches it).
# With YSP_BUILD_LAYOUT also ON it builds glyph runs; without, it is the core
# tool, which refuses them and builds every other kind to the same bytes, so
# a pack of tables, sounds and images does not need HarfBuzz. ysp/pack.h
# itself is a header in the list below and needs neither.
option(YSP_BUILD_PACK "Build the pack tool library, ypak and its tests (glyph runs need YSP_BUILD_LAYOUT)" OFF)

# ysp/screen.h needs SDL3 (3.2 or later). Without it only its SDL-free test is
# built; point SDL3_DIR at an SDL3 CMake package to build the rest. ANGLE is
# loaded at run time, so the build never needs it; YSP_ANGLE_DIR only copies
# libEGL.dll and libGLESv2.dll beside the programs that open a window.
find_package(SDL3 3.2 CONFIG QUIET)
set(YSP_ANGLE_DIR "" CACHE PATH "ANGLE's libEGL.dll and libGLESv2.dll, copied beside the ysp/screen.h programs")

# ysp/audio.h needs miniaudio.h (0.11.25, one file). YSP_MINIAUDIO_DIR names a
# directory that holds it; else YSP_FETCH_MINIAUDIO downloads the pinned file,
# checked by its hash. Off by default because it downloads, as
# YSP_TRACY_CHECK is; CI turns it on. Without either, only ysp/audio.h's
# miniaudio-free test is built.
set(YSP_MINIAUDIO_DIR "" CACHE PATH "Directory with miniaudio.h for ysp/audio.h")
option(YSP_FETCH_MINIAUDIO "Download miniaudio.h 0.11.25 for ysp/audio.h" OFF)
if(NOT YSP_MINIAUDIO_DIR AND YSP_FETCH_MINIAUDIO)
    include(FetchContent)
    FetchContent_Declare(miniaudio
        URL      https://raw.githubusercontent.com/mackron/miniaudio/0.11.25/miniaudio.h
        URL_HASH SHA256=ac7af4de748b7e26b777f37e01cee313a308a7296a3eb080e2906b320cc55c89
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(miniaudio)
    set(YSP_MINIAUDIO_DIR ${miniaudio_SOURCE_DIR})
endif()

# ysp/video.h decodes MPEG-1 with pl_mpeg.h (one MIT file). It has no release
# tags, so the pin is a commit and the file's SHA-256. YSP_PL_MPEG_DIR names a
# directory that holds it; else YSP_FETCH_PL_MPEG downloads it, and qoi.h,
# which ysp/video.h's test compares its own QOI encoder with. Off by default
# because it downloads; CI turns it on. Without it ysp/video.h builds with
# YVID_NO_PL_MPEG: frame sequences only.
set(YSP_PL_MPEG_DIR "" CACHE PATH "Directory with pl_mpeg.h for ysp/video.h")
option(YSP_FETCH_PL_MPEG "Download pl_mpeg.h (c871f2b) and qoi.h (ffb2d2c) for ysp/video.h" OFF)
if(NOT YSP_PL_MPEG_DIR AND YSP_FETCH_PL_MPEG)
    include(FetchContent)
    FetchContent_Declare(pl_mpeg
        URL      https://raw.githubusercontent.com/phoboslab/pl_mpeg/c871f2be022ece7ef4f64230b4fb8e1fb9eb6023/pl_mpeg.h
        URL_HASH SHA256=3a8cb30c83c2a1147719c30fe0c8b93da2987aa43140077c575b39aaa75fc2c9
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_Declare(qoi
        URL      https://raw.githubusercontent.com/phoboslab/qoi/ffb2d2cb74a1de60819b21b939f7209aa53e91c1/qoi.h
        URL_HASH SHA256=7de6fca1a285b1c20d38f2723dec8b774eb9f144edb9710800a95feeea09375a
        DOWNLOAD_NO_EXTRACT TRUE)
    FetchContent_MakeAvailable(pl_mpeg qoi)
    set(YSP_PL_MPEG_DIR ${pl_mpeg_SOURCE_DIR})
    set(YSP_QOI_DIR ${qoi_SOURCE_DIR})
endif()

ysp_link_rig_deps("set SDL3_DIR")
if(NOT TARGET ysp::miniaudio)
    message(STATUS "ysp/audio.h: miniaudio.h not found; set YSP_MINIAUDIO_DIR or YSP_FETCH_MINIAUDIO=ON")
endif()
if(NOT TARGET ysp::pl_mpeg)
    message(STATUS "ysp/video.h: pl_mpeg.h not found, so no MPEG-1; set YSP_PL_MPEG_DIR or YSP_FETCH_PL_MPEG=ON")
endif()

# --- library registry ---------------------------------------------------------
# This list is the single source for what gets built. To add ysp/<name>.h:
# append <name>, set its macro prefix, its ysp:: group (the consumer block at
# the top, which its programs link), the platforms it supports, and whether
# it has a Y<X>_NO_THREADS build. Compile checks come from
# tests/compile/<name>.{c,cpp} and examples from examples/<name>/*.c (built
# as <name>_<file>), both found by name and gated by platform. The transport headers include ysp/rt.h,
# which the include path above resolves from include/, so the
# compiler's own header dependencies rebuild every target after a ysp/rt.h
# edit. rt comes first so its compile checks run before its dependents'.
# The list's order, which is not the link groups': the transports (rt,
# parallel, serial), the adaptive methods (stair, quest, aep), tables and the
# trial sequencer (table, trials), the timeline (timeline) and color (color),
# then the rig headers.
# A header with a tests/adapt/<name>_test.c gets that self-checking
# program registered with ctest too, and one with YSP_ASYNC_<name> ON gets
# the compile check and that test built a second time with
# Y<X>_ASYNC defined, which is the opt-in layer that pulls in ysp/rt.h.
set(YSP_LIBS rt parallel serial stair quest aep table json pack trials timeline input response box device rigfile net color rdk outline screen audio gfx video)

# Examples that sit in the folder of the header they mainly show but need
# another header's build: path, program name (the file's own name, kept from
# the flat layout), and the library whose loop builds it with its group and
# dependencies ("-": built by its own block below). Every other example is
# examples/<lib>/<file>.c, built as <lib>_<file>.
set(YSP_EXAMPLES_ACROSS
    rdk/gfx_rdk.c              gfx
    rdk/gfx_rdk_bench.c        gfx
    timeline/gfx_trial.c       gfx
    layout/gfx_layout.c        -
    response/trial_keyboard.c  -
    response/trial_same_different.c -
    response/trial_srt.c       -
    response/trial_adjustment.c -
    response/trial_mouse_tracking.c -
    response/trial_audio_keyboard.c -
    response/trial_stop_signal.c -
    response/trial_2afc_adaptive.c -
    rdk/trial_rdk.c            -
    response/trial_stroop.c    -
    timeline/trial_sternberg.c -
    pack/trial_images.c        -
    pack/trial_images_assets.c -
    device/photodiode_check.c  -
    net/labstreamer_flip.c     -
    audio/av_sync.c            -
    response/trial_two_keyboards.c -)

set(YSP_PREFIX_rt          YRT)
set(YSP_GROUP_rt           core)
set(YSP_PLATFORMS_rt       Windows Linux Darwin)
set(YSP_THREADS_rt         ON)

set(YSP_PREFIX_parallel    YPAR)
set(YSP_GROUP_parallel     devices)
set(YSP_PLATFORMS_parallel Windows Linux)
set(YSP_THREADS_parallel   ON)

set(YSP_PREFIX_serial      YSER)
set(YSP_GROUP_serial       devices)
set(YSP_PLATFORMS_serial   Windows Linux Darwin)
set(YSP_THREADS_serial     ON)

# The adaptive-method headers are pure computation: every platform, no
# worker thread, so no NO_THREADS variant. quest and aep have an opt-in async
# layer on ysp/rt.h's pump, which YSP_ASYNC_<name> builds as a second variant.
set(YSP_PREFIX_stair       YST)
set(YSP_GROUP_stair        methods)
set(YSP_PLATFORMS_stair    Windows Linux Darwin)
set(YSP_THREADS_stair      OFF)

set(YSP_PREFIX_quest       YQST)
set(YSP_GROUP_quest        methods)
set(YSP_PLATFORMS_quest    Windows Linux Darwin)
set(YSP_THREADS_quest      OFF)
set(YSP_ASYNC_quest        ON)

set(YSP_PREFIX_aep         YAEP)
set(YSP_GROUP_aep          methods)
set(YSP_PLATFORMS_aep      Windows Linux Darwin)
set(YSP_THREADS_aep        OFF)
set(YSP_ASYNC_aep          ON)

# Typed tables (CSV and the pack's table block) include nothing; ysp/trials.h
# and later ysp/timeline.h and the pack tool read tables through it.
set(YSP_PREFIX_table       YTB)
set(YSP_GROUP_table        core)
set(YSP_PLATFORMS_table    Windows Linux Darwin)
set(YSP_THREADS_table      OFF)

# The strict JSON reader and canonical writer includes nothing; the pack
# tool and ysp/rigfile.h read and write JSON through it.
set(YSP_PREFIX_json        YJS)
set(YSP_GROUP_json         core)
set(YSP_PLATFORMS_json     Windows Linux Darwin)
set(YSP_THREADS_json       OFF)

# The pack reader includes nothing; it maps files through the OS (kernel32 on
# Windows, mmap on POSIX), which every program links already.
set(YSP_PREFIX_pack        YPAK)
set(YSP_GROUP_pack         core)
set(YSP_PLATFORMS_pack     Windows Linux Darwin)
set(YSP_THREADS_pack       OFF)

# The trial sequencer requires ysp/table.h beside it (its implementation
# includes ysp/table.h's, as ysp/gfx.h's does ysp/color.h's), and nothing else.
set(YSP_PREFIX_trials      YTR)
set(YSP_GROUP_trials       methods)
set(YSP_PLATFORMS_trials   Windows Linux Darwin)
set(YSP_THREADS_trials     OFF)

# The input event: types and inline adapters only, nothing to link. Its test
# reads ysp/screen.h's raw mouse struct and needs none of its deps.
set(YSP_PREFIX_input       YIN)
set(YSP_GROUP_input        devices)
set(YSP_PLATFORMS_input    Windows Linux Darwin)
set(YSP_THREADS_input      OFF)

# The response collector requires ysp/input.h beside it and nothing else;
# its test reads the structs of ysp/screen.h, ysp/audio.h and ysp/timeline.h
# and needs none of their deps.
set(YSP_PREFIX_response    YRSP)
set(YSP_GROUP_response     devices)
set(YSP_PLATFORMS_response Windows Linux Darwin)
set(YSP_THREADS_response   OFF)

# Box protocols are pure (decoders and query bytes, no I/O). The device
# layer runs them on reader threads over ysp/serial.h; it has no
# NO_THREADS variant, because manual mode (ydev_poll) is its threadless form.
set(YSP_PREFIX_box         YBOX)
set(YSP_GROUP_box          devices)
set(YSP_PLATFORMS_box      Windows Linux Darwin)
set(YSP_THREADS_box        OFF)

set(YSP_PREFIX_device      YDEV)
set(YSP_GROUP_device       devices)
set(YSP_PLATFORMS_device   Windows Linux Darwin)
set(YSP_THREADS_device     OFF)

# The rig profile sits on the device layer (roles, families, keys) and
# ysp/json.h. ysp/device.h has no NO_THREADS build, so this has none either.
set(YSP_PREFIX_rigfile     YRIG)
set(YSP_GROUP_rigfile      devices)
set(YSP_PLATFORMS_rigfile  Windows Linux Darwin)
set(YSP_THREADS_rigfile    OFF)

# LSL loads liblsl at run time, so it builds and its test runs without it.
# Manual mode (ynet_inlet_poll) is its threadless form.
set(YSP_PREFIX_net         YNET)
set(YSP_GROUP_net          devices)
set(YSP_PLATFORMS_net      Windows Linux Darwin)
set(YSP_THREADS_net        OFF)

# The timeline includes nothing either; it takes times as arguments.
set(YSP_PREFIX_timeline    YTL)
set(YSP_GROUP_timeline     core)
set(YSP_PLATFORMS_timeline Windows Linux Darwin)
set(YSP_THREADS_timeline   OFF)

# The color header is pure computation too: calibration, conversions, gamut.
set(YSP_PREFIX_color       YCOL)
set(YSP_GROUP_color        rig)
set(YSP_PLATFORMS_color    Windows Linux Darwin)
set(YSP_THREADS_color      OFF)

# Random dot kinematograms: pure computation on integers, nothing included.
# Its examples/rdk/bench.c runs on the CPU; examples/rdk/gfx_rdk*.c draw
# through gfx (YSP_EXAMPLES_ACROSS).
set(YSP_PREFIX_rdk         YRDK)
set(YSP_GROUP_rdk          rig)
set(YSP_PLATFORMS_rdk      Windows Linux Darwin)
set(YSP_THREADS_rdk        OFF)

# The outline builder is pure computation: fonts, paths, strokes, exact
# alpha and curve sets. Its test includes ysp/gfx.h (no SDL) for the curve
# set check only; examples/outline/bench.c uses ysp/rt.h's clock.
set(YSP_PREFIX_outline     YOL)
set(YSP_GROUP_outline      rig)
set(YSP_PLATFORMS_outline  Windows Linux Darwin)
set(YSP_THREADS_outline    OFF)

# The display header is the one with an external dependency. YSP_DEPS_<name>
# lists the targets its compile checks and examples need, which ysp::rig
# links when they exist; when one is missing, only its tests/adapt test
# (which must need none of them) is built.
set(YSP_PREFIX_screen      YSCR)
set(YSP_GROUP_screen       rig)
set(YSP_PLATFORMS_screen   Windows Linux Darwin)
set(YSP_THREADS_screen     OFF)
set(YSP_DEPS_screen        SDL3::SDL3)

# The audio header's one dependency is miniaudio.h, a header (see above).
set(YSP_PREFIX_audio       YAU)
set(YSP_GROUP_audio        rig)
set(YSP_PLATFORMS_audio    Windows Linux Darwin)
set(YSP_THREADS_audio      OFF)
set(YSP_DEPS_audio         ysp::miniaudio)

# The stimulus header sits on ysp/screen.h, so it needs what that needs. Its
# test builds without SDL and finds a GL ES 3.0 renderer at run time.
set(YSP_PREFIX_gfx         YGFX)
set(YSP_GROUP_gfx          rig)
set(YSP_PLATFORMS_gfx      Windows Linux Darwin)
set(YSP_THREADS_gfx        OFF)
set(YSP_DEPS_gfx           SDL3::SDL3)

# The video header sits on ysp/gfx.h and ysp/timeline.h; pl_mpeg is optional
# (see above and the block after the loop). Its test needs neither SDL3 nor
# pl_mpeg.
set(YSP_PREFIX_video       YVID)
set(YSP_GROUP_video        rig)
set(YSP_PLATFORMS_video    Windows Linux Darwin)
set(YSP_THREADS_video      OFF)
set(YSP_DEPS_video         SDL3::SDL3)

# -Wfloat-conversion: every time is int64_t ns, so seconds passed where ns
# go (ytl_wait(&q, 0.5)) must not compile quietly; MSVC's /W4 has C4244.
if(MSVC)
    set(YSP_WARNINGS /W4 /WX)
else()
    set(YSP_WARNINGS -Wall -Wextra -Wpedantic -Wshadow -Wfloat-conversion -Werror)
endif()

# ysp_add_program(<target> <src> <group>...): the groups are those of the
# headers the program includes. A program links them and nothing more, as a
# consumer would, so a library missing from a group fails that group's own
# programs instead of hiding behind the union.
function(ysp_add_program target src)
    if(NOT ARGN)
        message(FATAL_ERROR "ysp_add_program(${target}): name its ysp:: group")
    endif()
    add_executable(${target} ${src})
    foreach(group IN LISTS ARGN)
        target_link_libraries(${target} PRIVATE ysp::${group})
    endforeach()
    target_compile_options(${target} PRIVATE ${YSP_WARNINGS})
    set_target_properties(${target} PROPERTIES
        C_STANDARD 11 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF
        CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
    # The rig's programs may open a window, so they get the run-time DLLs
    # beside them. The tests among them use neither, which costs a copy.
    if("rig" IN_LIST ARGN)
        if(WIN32 AND TARGET SDL3::SDL3-shared)
            add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3-shared> $<TARGET_FILE_DIR:${target}>)
        endif()
        if(WIN32 AND YSP_ANGLE_DIR)
            add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${YSP_ANGLE_DIR}/libEGL.dll ${YSP_ANGLE_DIR}/libGLESv2.dll $<TARGET_FILE_DIR:${target}>)
        endif()
    endif()
endfunction()

if(YSP_BUILD_TESTS)
    enable_testing()
endif()

foreach(lib IN LISTS YSP_LIBS)
    if(NOT CMAKE_SYSTEM_NAME IN_LIST YSP_PLATFORMS_${lib})
        message(STATUS "ysp/${lib}.h: not supported on ${CMAKE_SYSTEM_NAME}, skipped")
        continue()
    endif()
    set(deps_ok TRUE)
    foreach(dep IN LISTS YSP_DEPS_${lib})
        if(NOT TARGET ${dep})
            set(deps_ok FALSE)
        endif()
    endforeach()
    if(NOT deps_ok)
        message(STATUS "ysp/${lib}.h: ${YSP_DEPS_${lib}} not found; only its tests/adapt test is built")
    endif()
    set(group ${YSP_GROUP_${lib}})
    if(NOT group)
        message(FATAL_ERROR "ysp/${lib}.h: set YSP_GROUP_${lib}")
    endif()

    # --- compile checks: every header must build clean as C11, as C++17, and
    #     (when it has threading) with it disabled.
    if(YSP_BUILD_TESTS AND deps_ok)
        set(variants c cxx)
        ysp_add_program(compile_${lib}_c   tests/compile/${lib}.c   ${group})
        ysp_add_program(compile_${lib}_cxx tests/compile/${lib}.cpp ${group})
        if(YSP_THREADS_${lib})
            ysp_add_program(compile_${lib}_nothreads tests/compile/${lib}.c ${group})
            target_compile_definitions(compile_${lib}_nothreads PRIVATE ${YSP_PREFIX_${lib}}_NO_THREADS)
            list(APPEND variants nothreads)
        endif()
        foreach(variant IN LISTS variants)
            add_test(NAME compile_${lib}_${variant} COMMAND compile_${lib}_${variant})
        endforeach()
        # Extra checks named tests/compile/<lib>_<what>.cpp (screen_com.cpp
        # checks the COM declarations against the Windows SDK).
        file(GLOB lib_extra_checks CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tests/compile/${lib}_*.cpp)
        foreach(src IN LISTS lib_extra_checks)
            get_filename_component(name ${src} NAME_WE)
            ysp_add_program(compile_${name} ${src} ${group})
            add_test(NAME compile_${name} COMMAND compile_${name})
        endforeach()
        # ysp/timeline.h's op table macros are designated initializers,
        # which C++ has from C++20 on; g++ warns about every field they
        # leave out, though zero is the default.
        if(TARGET compile_timeline_ops)
            set_target_properties(compile_timeline_ops PROPERTIES CXX_STANDARD 20)
            if(NOT MSVC)
                target_compile_options(compile_timeline_ops PRIVATE -Wno-missing-field-initializers)
            endif()
        endif()
        # ysp/color.h's value macros (YCOL_DKL(...)) are the same kind.
        if(TARGET compile_color_macros)
            set_target_properties(compile_color_macros PROPERTIES CXX_STANDARD 20)
            if(NOT MSVC)
                target_compile_options(compile_color_macros PRIVATE -Wno-missing-field-initializers)
            endif()
        endif()
        # ysp/outline.h's desc structs set by designated initializers.
        if(TARGET compile_outline_desc)
            set_target_properties(compile_outline_desc PROPERTIES CXX_STANDARD 20)
            if(NOT MSVC)
                target_compile_options(compile_outline_desc PRIVATE -Wno-missing-field-initializers)
            endif()
        endif()
    endif()

    # --- verification tests: tests/adapt/<lib>_test.c is self-checking
    #     (exit 0 on pass) and needs no hardware, so ctest runs it, and the
    #     sanitizer job in CI runs it instrumented.
    if(YSP_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/adapt/${lib}_test.c)
        ysp_add_program(test_${lib} tests/adapt/${lib}_test.c ${group})
        add_test(NAME test_${lib} COMMAND test_${lib})
        if(YSP_ASYNC_${lib})
            ysp_add_program(compile_${lib}_async tests/compile/${lib}.c ${group})
            target_compile_definitions(compile_${lib}_async PRIVATE ${YSP_PREFIX_${lib}}_ASYNC)
            add_test(NAME compile_${lib}_async COMMAND compile_${lib}_async)
            ysp_add_program(test_${lib}_async tests/adapt/${lib}_test.c ${group})
            target_compile_definitions(test_${lib}_async PRIVATE ${YSP_PREFIX_${lib}}_ASYNC)
            add_test(NAME test_${lib}_async COMMAND test_${lib}_async)
        endif()
    endif()

    # --- loopback tests: tests/loopback/<lib>_loopback.c, run by hand with
    #     the device names of a wired pair. Built, never registered with ctest.
    if(YSP_BUILD_LOOPBACK AND deps_ok AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/loopback/${lib}_loopback.c)
        ysp_add_program(${lib}_loopback tests/loopback/${lib}_loopback.c ${group})
    endif()

    # --- examples: examples/<lib>/<file>.c, one program each, <lib>_<file>;
    #     and the YSP_EXAMPLES_ACROSS this library builds, under their own name.
    if(YSP_BUILD_EXAMPLES AND deps_ok)
        file(GLOB lib_examples CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/examples/${lib}/*.c)
        foreach(src IN LISTS lib_examples)
            get_filename_component(name ${src} NAME_WE)
            if(NOT "${lib}/${name}.c" IN_LIST YSP_EXAMPLES_ACROSS)
                ysp_add_program(${lib}_${name} ${src} ${group})
            endif()
        endforeach()
        set(across ${YSP_EXAMPLES_ACROSS})
        while(across)
            list(POP_FRONT across path owner)
            if(owner STREQUAL lib)
                get_filename_component(name ${path} NAME_WE)
                ysp_add_program(${name} examples/${path} ${group})
            endif()
        endwhile()
    endif()
endforeach()

# The audio loopback test opens WASAPI's loopback capture itself.
if(TARGET audio_loopback AND WIN32)
    target_link_libraries(audio_loopback PRIVATE ole32)
endif()
# The WASAPI period probe calls IAudioClient3 itself, not through miniaudio.
if(TARGET audio_wasapi_periods AND WIN32)
    target_link_libraries(audio_wasapi_periods PRIVATE ole32)
endif()
# The loop examples set the endpoints' volume and mute for an acoustic run.
foreach(_t audio_loopback_latency audio_first_sound audio_voice_rt)
    if(TARGET ${_t} AND WIN32)
        target_link_libraries(${_t} PRIVATE ole32)
    endif()
endforeach()

# ysp/net.h loads liblsl at run time and never links it. tools/vendor_lsl.py
# fetches a pinned release for the tests into third_party/liblsl/<platform>/.
# With it: the loopback test loads that copy by default, and
# tests/compile/lsl_abi.cpp checks the header's declarations against its
# lsl_c.h. Without it only the fake-liblsl unit test runs.
set(YSP_LSL_DIR "" CACHE PATH "liblsl for ysp/net.h's tests: the folder with lsl.dll or liblsl.so and lsl_c.h")
if(NOT YSP_LSL_DIR)
    if(WIN32)
        set(_lsl_plat win_amd64)
    else()
        set(_lsl_plat noble_amd64)
        if(EXISTS /etc/os-release)
            file(READ /etc/os-release _lsl_rel)
            if(_lsl_rel MATCHES "VERSION_CODENAME=jammy")
                set(_lsl_plat jammy_amd64)
            endif()
        endif()
    endif()
    set(YSP_LSL_DIR ${CMAKE_CURRENT_SOURCE_DIR}/third_party/liblsl/${_lsl_plat})
endif()
if(WIN32)
    set(_lsl_lib ${YSP_LSL_DIR}/lsl.dll)
else()
    set(_lsl_lib ${YSP_LSL_DIR}/liblsl.so)
endif()
if(EXISTS ${_lsl_lib} AND EXISTS ${YSP_LSL_DIR}/lsl_c.h)
    if(TARGET net_loopback)
        target_compile_definitions(net_loopback PRIVATE YNET_LOOPBACK_LIB="${_lsl_lib}")
    endif()
    if(YSP_BUILD_TESTS)
        ysp_add_program(compile_lsl_abi tests/compile/lsl_abi.cpp devices)
        target_include_directories(compile_lsl_abi SYSTEM PRIVATE ${YSP_LSL_DIR})
        add_test(NAME compile_lsl_abi COMMAND compile_lsl_abi)
    endif()
    message(STATUS "ysp/net.h: tests use liblsl at ${_lsl_lib}")
elseif(CMAKE_SYSTEM_NAME IN_LIST YSP_PLATFORMS_net)
    message(STATUS "ysp/net.h: no liblsl in ${YSP_LSL_DIR}, so only the fake-liblsl unit test runs; "
                   "fetch it with: uv run --no-project python tools/vendor_lsl.py --write")
endif()

# ysp/input.h and ysp/response.h with SDL3: their tests again with the SDL
# adapter (SDL's headers only, nothing linked), and the examples of
# YSP_EXAMPLES_ACROSS whose groups are their own: the response trials (on
# ysp/gfx.h and the methods) and the photodiode check (a device and a
# screen).
if(TARGET SDL3::Headers AND YSP_BUILD_TESTS)
    ysp_add_program(test_response_sdl tests/adapt/response_test.c devices)
    target_compile_definitions(test_response_sdl PRIVATE YRSP_TEST_SDL)
    target_link_libraries(test_response_sdl PRIVATE SDL3::Headers)
    add_test(NAME test_response_sdl COMMAND test_response_sdl)
    ysp_add_program(test_input_sdl tests/adapt/input_test.c devices)
    target_compile_definitions(test_input_sdl PRIVATE YIN_TEST_SDL)
    target_link_libraries(test_input_sdl PRIVATE SDL3::Headers)
    add_test(NAME test_input_sdl COMMAND test_input_sdl)
endif()
if(TARGET SDL3::SDL3 AND YSP_BUILD_EXAMPLES)
    # The jsPsych-style trials of examples/response/ (docs/response.md,
    # "What the examples show"); the two with a sound need miniaudio.
    foreach(_t keyboard same_different srt adjustment mouse_tracking 2afc_adaptive two_keyboards)
        ysp_add_program(trial_${_t} examples/response/trial_${_t}.c rig methods)
    endforeach()
    # PsychoPy's dots.py as a trial (docs/rig_spec.md 15.5): RDK direction
    # discrimination, each row with the field's replay digest.
    ysp_add_program(trial_rdk examples/rdk/trial_rdk.c rig methods)
    # The web demos (docs/web.md): examples/web/build.sh compiles them with
    # emcc; here they build natively, so --sim checks them on CI.
    ysp_add_program(web_tutorial examples/web/tutorial.c rig methods)
    ysp_add_program(web_rdk_staircase examples/web/rdk_staircase.c rig methods)
    ysp_add_program(web_selftest examples/web/selftest.c rig)
    if(TARGET ysp::miniaudio)
        foreach(_t audio_keyboard stop_signal)
            ysp_add_program(trial_${_t} examples/response/trial_${_t}.c rig methods)
        endforeach()
        # PTB's PsychPortAudioTimingTest (docs/rig_spec.md 16.6): a flash and
        # a click for one time, the records, and line boards in hardware mode.
        ysp_add_program(audio_av_sync examples/audio/av_sync.c rig devices)
    endif()
    # PsychoPy's Sternberg demo (docs/rig_spec.md 15.5): a timed sequence as
    # an op table, digits from ysp/outline.h. trial_stroop and trial_images
    # are in pack/CMakeLists.txt: they need the layout library and ypak.
    ysp_add_program(trial_sternberg examples/timeline/trial_sternberg.c rig methods)
    # The MCU photodiode on ysp/device.h, its events over ysp/screen.h's
    # input bridge: the first real device's hand test.
    ysp_add_program(photodiode_check examples/device/photodiode_check.c rig devices)
    # The LabStreamer against flip records over LSL (docs/net.md): the hand
    # test of ysp/net.h.
    ysp_add_program(net_labstreamer_flip examples/net/labstreamer_flip.c rig devices)
endif()

# ysp/video.h: ysp::rig carries pl_mpeg when it was found, else these build
# without MPEG-1. The test has its own scripted decoder; it compares QOI
# bytes with qoi.h when that was fetched.
if(NOT TARGET ysp::pl_mpeg)
    foreach(t compile_video_c compile_video_cxx video_play video_bench)
        if(TARGET ${t})
            target_compile_definitions(${t} PRIVATE YVID_NO_PL_MPEG)
        endif()
    endforeach()
endif()
if(TARGET test_video AND YSP_QOI_DIR)
    target_include_directories(test_video PRIVATE ${YSP_QOI_DIR})
    target_compile_definitions(test_video PRIVATE YVID_TEST_QOI_REF)
endif()

if(YSP_BUILD_LAYOUT)
    # The vendored trees are not in git (third_party/README.md): fetch them
    # once. Configure never downloads by itself.
    foreach(_tp harfbuzz/src/harfbuzz.cc sheenbidi/Source/SheenBidi.c libunibreak/src/linebreak.c
                budouxc/src/budoux.c skribidi/src/skb_layout.c)
        if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/${_tp})
            message(FATAL_ERROR "YSP_BUILD_LAYOUT: third_party/${_tp} is missing. Fetch and verify the "
                "layout sources once, from the repository root:
"
                "    uv run --no-project python tools/vendor_layout.py --write
"
                "(or python3 tools/vendor_layout.py --write; it needs git for the patches)")
        endif()
    endforeach()
    add_subdirectory(third_party)
endif()
if(YSP_BUILD_PACK AND NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/lodepng/lodepng.cpp)
    message(FATAL_ERROR "YSP_BUILD_PACK: third_party/lodepng is missing. Fetch and verify it once, from the "
        "repository root:\n"
        "    uv run --no-project python tools/vendor_pack.py --write\n"
        "(or python3 tools/vendor_pack.py --write)")
endif()
if(YSP_BUILD_LAYOUT OR YSP_BUILD_PACK)
    add_subdirectory(pack)
endif()

if(YSP_TRACY_CHECK AND YSP_BUILD_TESTS)
    include(FetchContent)
    # The tag ysp/rt.h's STATUS names. On demand and silent: the check runs
    # on CI, where nothing connects and nothing should broadcast.
    set(TRACY_ENABLE       ON CACHE BOOL "" FORCE)   # Tracy's own default is OFF
    set(TRACY_ON_DEMAND    ON CACHE BOOL "" FORCE)
    set(TRACY_NO_BROADCAST ON CACHE BOOL "" FORCE)
    FetchContent_Declare(tracy
        GIT_REPOSITORY https://github.com/wolfpld/tracy.git
        GIT_TAG        v0.14.1
        GIT_SHALLOW    TRUE)
    FetchContent_MakeAvailable(tracy)
    ysp_add_program(compile_rt_tracy tests/compile/rt.c core)
    target_compile_definitions(compile_rt_tracy PRIVATE YRT_TRACY)
    target_link_libraries(compile_rt_tracy PRIVATE Tracy::TracyClient)
    add_test(NAME compile_rt_tracy COMMAND compile_rt_tracy)
endif()
