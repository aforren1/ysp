// gl_viewports.h - the fence between the window's GL context, where the
// preview's gfx draws its output texture inside ui_frame(), and the
// contexts of ImGui's other viewports (the popped-out preview), which
// sample that texture (docs/designer.md, "Preview size (2026-11-23)").
//
// GL ES 3.0, appendix D: a change that one context makes to a shared object
// is sure to be seen in another only after a Finish, or a fence that the
// other waits on, and a bind there. ImGui binds the texture at each draw.
// The probe (tests/probe/designer_viewports/fence_probe.c) saw no torn frame
// without the fence on radeonsi, iris and llvmpipe; the fence costs about
// 0.04 ms a frame on the RX 6600 and only runs while a second viewport is
// there. Include after <GLES3/gl3.h> and imgui.h; native builds only.
#pragma once

namespace glvp {

inline GLsync& fence() {
    static GLsync f = nullptr;
    return f;
}

typedef void (*RenderWindowFn)(ImGuiViewport*, void*);
inline RenderWindowFn& backend_render() {
    static RenderWindowFn fn = nullptr;
    return fn;
}

// On the viewport's own context (Platform_RenderWindow made it current):
// the GPU waits for the main context's draw, then the backend draws.
inline void render_window(ImGuiViewport* vp, void* arg) {
    if (fence()) glWaitSync(fence(), 0, GL_TIMEOUT_IGNORED);
    if (backend_render()) backend_render()(vp, arg);
}

// After the renderer backend's init: wraps its Renderer_RenderWindow.
inline void install() {
    ImGuiPlatformIO& p = ImGui::GetPlatformIO();
    backend_render() = p.Renderer_RenderWindow;
    p.Renderer_RenderWindow = render_window;
}

// On the main context, after its draw and before
// ImGui::RenderPlatformWindowsDefault(). The flush puts the fence in the
// GPU's queue before another context waits on it.
inline void begin() {
    if (ImGui::GetPlatformIO().Viewports.Size <= 1) return;
    fence() = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    glFlush();
}

// With the main context current again.
inline void end() {
    if (!fence()) return;
    glDeleteSync(fence());
    fence() = nullptr;
}

} // namespace glvp
