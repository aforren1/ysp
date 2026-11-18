// Route (b) of docs/filly_probe.md question 1: Filament's OpenGL backend
// built for GL ES (FILAMENT_SUPPORTS_EGL_ON_LINUX, FILAMENT_LINUX_IS_MOBILE)
// shares an EGL ES 3.0 context that ysp's gfx draws with. No filly: filly's
// interop code is desktop GL only, so this is Filament and gltfio directly.
//
// ysp runs headless on that context (ygfx_desc.gl_proc), as
// tests/adapt/gfx_headless.h does, but on eglGetDisplay(EGL_DEFAULT_DISPLAY),
// because Filament's PlatformEGL takes that display and EGL shares objects
// only between contexts of one display. Filament renders filly's sample horse
// into a texture ysp made; ysp draws it beside a gabor and reads the output.
//
//   gles_share GLB OUT.ppm [frames]
#include "ysp/gfx.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include <backend/platforms/PlatformEGLHeadless.h>
#include <filament/Camera.h>
#include <filament/Engine.h>
#include <filament/Fence.h>
#include <filament/LightManager.h>
#include <filament/RenderTarget.h>
#include <filament/Renderer.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/Texture.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/Viewport.h>
#include <gltfio/Animator.h>
#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
// The archive of this build tree, not the release SDK header of the same name.
#include <materials/uberarchive.h>
#include <utils/EntityManager.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
#include <vector>

using namespace filament;
using namespace filament::math;

static int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

static yscr_proc egl_proc(void*, const char* name) { return reinterpret_cast<yscr_proc>(eglGetProcAddress(name)); }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: gles_share GLB OUT.ppm [frames]\n"); return 2; }
    const int W = 1024, H = 768, TW = 512, TH = 512;
    int frames = argc > 3 ? atoi(argv[3]) : 600;

    // --- ysp's context: EGL, GL ES 3.0, a pbuffer as framebuffer 0 ---------------------
    EGLDisplay dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    EGLint maj = 0, min = 0;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "eglInitialize 0x%x\n", eglGetError()); return 1; }
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLint ca[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
                    EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    eglChooseConfig(dpy, ca, &cfg, 1, &n);
    EGLint xa[] = { EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
    EGLint pa[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pa);
    if (!ctx || !surf || !eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "EGL context 0x%x\n", eglGetError()); return 1; }
    printf("ysp context: %s, %s (EGL %d.%d, %s)\n", glGetString(GL_VERSION), glGetString(GL_RENDERER), maj, min,
           eglQueryString(dpy, EGL_VENDOR));

    static ygfx_gfx gfx;
    ygfx_desc gd;
    memset(&gd, 0, sizeof gd);
    gd.gl_proc = egl_proc;
    gd.width = W;
    gd.height = H;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx: %s\n", ygfx_error(&gfx)); return 1; }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, TW, TH);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glBindTexture(GL_TEXTURE_2D, 0);
    glFinish();  // the texture must exist before Filament's context sees it
    ygfx_reset_state(&gfx);
    ygfx_import_desc id;
    memset(&id, 0, sizeof id);
    id.kind = YGFX_IMPORT_GL;
    id.w = TW; id.h = TH;
    id.format = YGFX_RGBA8;
    // Filament's default color grading writes sRGB-encoded values into a non-sRGB target.
    id.enc.matrix = YGFX_MATRIX_RGB; id.enc.range = YGFX_RANGE_FULL; id.enc.transfer = YGFX_TRC_SRGB;
    id.enc.primaries = YGFX_PRIM_DEVICE;
    id.gl_tex[0] = tex;
    ygfx_tex ytex = ygfx_texture_import(&gfx, &id);
    if (!ytex.id) { fprintf(stderr, "import: %s\n", ygfx_error(&gfx)); return 1; }

    // --- Filament, GL ES backend, sharing ysp's context ---------------------------------
    int64_t t0 = now_ns();
    auto* platform = new backend::PlatformEGLHeadless();
    Engine* engine = Engine::Builder().backend(Engine::Backend::OPENGL).platform(platform)
                         .sharedContext(ctx).build();
    if (!engine) { fprintf(stderr, "Filament: no engine (sharedContext refused?)\n"); return 1; }
    printf("Filament engine with a shared ES context: %.1f ms, backend %d, feature level %d\n", (now_ns() - t0) / 1e6,
           int(engine->getBackend()), int(engine->getActiveFeatureLevel()));
    SwapChain* sc = engine->createSwapChain(1, 1, 0);
    Renderer* renderer = engine->createRenderer();
    Texture* color = Texture::Builder().width(TW).height(TH).levels(1).format(Texture::InternalFormat::RGBA8)
                         .usage(Texture::Usage::COLOR_ATTACHMENT | Texture::Usage::SAMPLEABLE)
                         .import(intptr_t(tex)).build(*engine);
    Texture* depth = Texture::Builder().width(TW).height(TH).levels(1).format(Texture::InternalFormat::DEPTH24)
                         .usage(Texture::Usage::DEPTH_ATTACHMENT).build(*engine);
    RenderTarget* rt = RenderTarget::Builder().texture(RenderTarget::AttachmentPoint::COLOR, color)
                           .texture(RenderTarget::AttachmentPoint::DEPTH, depth).build(*engine);
    Scene* scene = engine->createScene();
    View* view = engine->createView();
    utils::Entity cam_e = utils::EntityManager::get().create();
    Camera* cam = engine->createCamera(cam_e);
    view->setScene(scene);
    view->setCamera(cam);
    view->setRenderTarget(rt);
    view->setViewport({0, 0, uint32_t(TW), uint32_t(TH)});
    Skybox* sky = Skybox::Builder().color({0.2f, 0.2f, 0.25f, 1.0f}).build(*engine);
    scene->setSkybox(sky);
    utils::Entity sun = utils::EntityManager::get().create();
    LightManager::Builder(LightManager::Type::DIRECTIONAL).intensity(100000).direction({-1, -1, -2})
        .castShadows(false).build(*engine, sun);
    scene->addEntity(sun);

    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> glb((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    gltfio::MaterialProvider* mats = gltfio::createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
    gltfio::AssetLoader* loader = gltfio::AssetLoader::create({engine, mats, nullptr});
    gltfio::FilamentAsset* asset = loader->createAsset(glb.data(), uint32_t(glb.size()));
    if (!asset) { fprintf(stderr, "gltfio: no asset\n"); return 1; }
    gltfio::ResourceLoader resources({engine, nullptr, true});
    resources.loadResources(asset);
    asset->releaseSourceData();
    scene->addEntities(asset->getEntities(), asset->getEntityCount());
    Aabb box = asset->getBoundingBox();
    float3 c = box.center(), e = box.extent();
    float r = length(e);
    cam->setProjection(30.0, double(TW) / TH, 0.05 * r, 20.0 * r);
    cam->lookAt(c + normalize(float3{1, 0.3f, 1}) * (r * 2.2f), c, {0, 1, 0});
    gltfio::Animator* anim = asset->getInstance()->getAnimator();
    auto& tm = engine->getTransformManager();
    auto root = tm.getInstance(asset->getRoot());

    // --- frames ----------------------------------------------------------------------------
    ygfx_gabor_desc gab;
    memset(&gab, 0, sizeof gab);
    gab.x = -300; gab.sf = 1 / 32.0f; gab.sigma = 40; gab.contrast = 0.5f;
    ygfx_stim g = ygfx_gabor(&gab);
    std::vector<double> submit, wait, gfx_ms;
    std::vector<uint8_t> out(size_t(W) * H * 4);
    for (int i = 0; i < frames; i++) {
        double t = i / 60.0;
        int64_t a = now_ns();
        tm.setTransform(root, mat4f::rotation(float(t * M_PI / 2), float3{0, 1, 0}));
        if (anim->getAnimationCount()) { anim->applyAnimation(0, float(fmod(t, anim->getAnimationDuration(0)))); anim->updateBoneMatrices(); }
        if (renderer->beginFrame(sc)) {
            renderer->render(view);
            renderer->endFrame();
        } else {
            renderer->render(view);
        }
        int64_t b = now_ns();
        // No GPU-side wait exists in Filament's public API for another context: a CPU wait
        // for Filament's work (filly adds glFenceSync/glWaitSync through its platform subclass).
        Fence::waitAndDestroy(engine->createFence());
        int64_t c2 = now_ns();
        yscr_frame f;
        memset(&f, 0, sizeof f);
        f.index = i; f.onset = int64_t(t * 1e9); f.period = 16666667;
        ygfx_reset_state(&gfx);
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &g);
        ygfx_image_desc imd;
        memset(&imd, 0, sizeof imd);
        imd.tex = ytex; imd.x = 150; imd.linear = true;
        ygfx_stim im = ygfx_image(&gfx, &imd);
        ygfx_draw(&gfx, &im);
        ygfx_end(&gfx);
        if (i == frames - 1) ygfx_read_output(&gfx, 0, 0, W, H, out.data());
        glFlush();
        int64_t d = now_ns();
        if (i >= 10) { submit.push_back((b - a) / 1e6); wait.push_back((c2 - b) / 1e6); gfx_ms.push_back((d - c2) / 1e6); }
    }
    auto p = [](std::vector<double> v, const char* name) {
        std::sort(v.begin(), v.end());
        printf("  %-26s p50 %.3f  p99 %.3f  max %.3f ms (n %zu)\n", name, v[v.size() / 2], v[size_t(0.99 * (v.size() - 1))], v.back(), v.size());
    };
    printf("%d frames, unpaced (headless, no vblank):\n", frames);
    p(submit, "Filament submit (CPU)");
    p(wait, "Fence wait (CPU)");
    p(gfx_ms, "gfx begin..end (CPU)");
    long model_px = 0;
    // The square's corner is Filament's clear color; the horse is what differs from it.
    const uint8_t* bg = &out[(size_t(H / 2 - 250) * W + (W / 2 + 150 - 250)) * 4];
    for (int y = H / 2 - 256; y < H / 2 + 256; y++)
        for (int x = W / 2 + 150 - 256; x < W / 2 + 150 + 256; x++) {
            const uint8_t* q = &out[(size_t(y) * W + x) * 4];
            if (std::abs(q[0] - bg[0]) + std::abs(q[1] - bg[1]) + std::abs(q[2] - bg[2]) > 12) model_px++;
        }
    printf("pixels in the model's square that are not background: %ld\n", model_px);
    if (FILE* f = fopen(argv[2], "wb")) {
        fprintf(f, "P6 %d %d 255\n", W, H);
        for (size_t i = 0; i < size_t(W) * H; i++) fwrite(&out[i * 4], 1, 3, f);
        fclose(f);
    }
    scene->removeEntities(asset->getEntities(), asset->getEntityCount());
    loader->destroyAsset(asset);
    mats->destroyMaterials();
    delete mats;
    gltfio::AssetLoader::destroy(&loader);
    engine->destroy(view);
    engine->destroy(scene);
    engine->destroy(sky);
    engine->destroy(rt);
    engine->destroy(color);
    engine->destroy(depth);
    engine->destroyCameraComponent(cam_e);
    engine->destroy(sun);
    engine->destroy(renderer);
    engine->destroy(sc);
    Engine::destroy(&engine);
    ygfx_close(&gfx);
    return 0;
}
