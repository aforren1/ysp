# Standalone build of the WASAPI exclusive-mode probe (docs/audio.md,
# "Exclusive mode compared with cave-audio").
#   cmake -S tests/probe/wasapi_exclusive -B build/wasapi_exclusive
#   cmake --build build/wasapi_exclusive --config Release
# It is not part of the main project: it takes the audio device away from
# every other program while it runs.
cmake_minimum_required(VERSION 3.20)
project(wasapi_exclusive_probe C)
if(NOT WIN32)
  message(FATAL_ERROR "wasapi_exclusive_probe is Windows-only")
endif()
add_executable(wasapi_exclusive_probe wasapi_exclusive_probe.c)
set_target_properties(wasapi_exclusive_probe PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED ON)
target_link_libraries(wasapi_exclusive_probe PRIVATE ole32 winmm avrt)
if(MSVC)
  target_compile_options(wasapi_exclusive_probe PRIVATE /W4)
else()
  target_compile_options(wasapi_exclusive_probe PRIVATE -Wall -Wextra)
endif()
