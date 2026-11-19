// imconfig_designer_te.h - Dear ImGui's configuration for test_designer_te
// only: the designer's own (designer/app/imconfig_designer.h) plus the Test
// Engine's hooks. The shipped designer never sees this file
// (docs/designer.md, "UI tests with the Test Engine").
#pragma once
#include "imconfig_designer.h"

// No screen captures: the runner has no renderer, and capture would need
// stb_image_write, which the vendor script does not fetch.
#define IMGUI_TEST_ENGINE_ENABLE_CAPTURE 0
// The engine runs each test function as a coroutine. Its std::thread
// implementation is the one it ships; a test-only target may use the C++
// runtime.
#define IMGUI_TEST_ENGINE_ENABLE_COROUTINE_STDTHREAD_IMPL 1
#include "imgui_te_imconfig.h"
