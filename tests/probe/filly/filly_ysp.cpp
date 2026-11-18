// filly as a ysp Renderer extension: the native probe (docs/filly_probe.md,
// questions 1 to 3). A ysp screen (GLX_OML, windowed) draws a gabor and a
// filly glTF model that turns with the predicted onset, through one of:
//
//   --mode none            the same loop without filly (the baseline)
//   --mode shared          route (a): Filament's GL 4.1 context shares ysp's
//                          GL ES 3.0 GLX context; filly renders into a texture
//                          ysp made, which ygfx_texture_import() wraps. No copy.
//   --mode readback        route (d): an offscreen filly renderer (its own EGL
//                          context), RenderTarget::read(), ygfx_texture_update().
//   --mode readback-async  route (d), pipelined: read frame N-1 after
//                          submitting frame N; one frame of latency.
//
// Other options: --size WxH (filly's target, default 512x512), --frames N
// (default 3600), --csv PATH (per-frame rows), --check (question 3: pixel
// checks of the output instead of a timing run), --model PATH (a GLB file).
//
// The time filly takes is charged to YSCR_PHASE_SCRIPT, which this loop
// does not otherwise use: section 12's "extension phase" does not exist yet.
#include "ysp/gfx.h"
#include "renderer.h"

#include <GLES3/gl3.h>
#include <sched.h>
#include <dirent.h>
#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {

int64_t now_ns() {
    timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return int64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

struct ThreadInfo {
    int tid;
    std::string name;
    int policy;
    double cpu_s;
};

// Every thread of the process: name, scheduling policy and CPU time, to see
// what Filament starts and what it costs.
std::vector<ThreadInfo> threads() {
    std::vector<ThreadInfo> out;
    DIR* d = opendir("/proc/self/task");
    if (!d) return out;
    long hz = sysconf(_SC_CLK_TCK);
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ThreadInfo t{atoi(e->d_name), "", sched_getscheduler(atoi(e->d_name)), 0};
        char path[64], buf[1024];
        snprintf(path, sizeof path, "/proc/self/task/%s/comm", e->d_name);
        if (FILE* f = fopen(path, "r")) {
            if (fgets(buf, sizeof buf, f)) { buf[strcspn(buf, "\n")] = 0; t.name = buf; }
            fclose(f);
        }
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        if (FILE* f = fopen(path, "r")) {
            if (fgets(buf, sizeof buf, f)) {
                const char* p = strrchr(buf, ')');
                unsigned long ut = 0, st = 0;
                // Fields after the name: state is 3, utime 14, stime 15.
                if (p && sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %lu %lu", &ut, &st) == 2)
                    t.cpu_s = double(ut + st) / double(hz);
            }
            fclose(f);
        }
        out.push_back(t);
    }
    closedir(d);
    std::sort(out.begin(), out.end(), [](const ThreadInfo& a, const ThreadInfo& b) { return a.tid < b.tid; });
    return out;
}

struct Pct {
    double p50, p99, max;
    size_t n;
};
Pct pct(std::vector<double> v) {
    if (v.empty()) return {0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    auto at = [&](double q) { return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))]; };
    return {at(0.5), at(0.99), v.back(), v.size()};
}

filly::Matrix rot_y(double deg) {
    double a = deg * M_PI / 180.0, c = std::cos(a), s = std::sin(a);
    // Column-major, as glTF and Filament store matrices.
    return {float(c), 0, float(-s), 0, 0, 1, 0, 0, float(s), 0, float(c), 0, 0, 0, 0, 1};
}

struct Gl {
    void (*GenTextures)(GLsizei, GLuint*);
    void (*BindTexture)(GLenum, GLuint);
    void (*TexStorage2D)(GLenum, GLsizei, GLenum, GLsizei, GLsizei);
    void (*TexParameteri)(GLenum, GLenum, GLint);
    void (*DeleteTextures)(GLsizei, const GLuint*);
    GLenum (*GetError)();
};

template <class F> void load(F& fn, yscr_screen* s, const char* name) { fn = reinterpret_cast<F>(yscr_gl_proc(s, name)); }

}  // namespace

int main(int argc, char** argv) {
    std::string mode = "shared", csv, model_path = "samples/horse.glb";
    int tw = 512, th = 512, check = 0, win_w = 1024, win_h = 768;
    int64_t frames = 3600;
    std::string fit = "sphere";
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "--mode" && i + 1 < argc) mode = argv[++i];
        else if (a == "--size" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &tw, &th);
        else if (a == "--frames" && i + 1 < argc) frames = atoll(argv[++i]);
        else if (a == "--csv" && i + 1 < argc) csv = argv[++i];
        else if (a == "--model" && i + 1 < argc) model_path = argv[++i];
        else if (a == "--window" && i + 1 < argc) sscanf(argv[++i], "%dx%d", &win_w, &win_h);
        else if (a == "--check") check = 1;
        else if (a == "--fit" && i + 1 < argc) fit = argv[++i];
        else { fprintf(stderr, "usage: filly_ysp [--mode none|shared|readback|readback-async] [--size WxH] "
                               "[--frames N] [--csv PATH] [--model GLB] [--window WxH] [--check]\n"); return 2; }
    }
    const bool shared = mode == "shared", readback = mode.rfind("readback", 0) == 0, async = mode == "readback-async";
    if (!shared && !readback && mode != "none") { fprintf(stderr, "bad --mode\n"); return 2; }

    static yscr_screen scr;
    static ygfx_gfx gfx;
    yscr_desc sd;
    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    sd.window_w = win_w;
    sd.window_h = win_h;
    sd.backend = YSCR_BACKEND_GLX_OML;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&scr)); return 1; }
    ygfx_desc gd;
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx: %s\n", ygfx_error(&gfx)); return 1; }
    char line[1024];
    yscr_describe(&scr, line, sizeof line);
    printf("screen: %s\n", line);
    ygfx_describe(&gfx, line, sizeof line);
    printf("gfx: %s\n", line);

    Gl gl;
    load(gl.GenTextures, &scr, "glGenTextures");
    load(gl.BindTexture, &scr, "glBindTexture");
    load(gl.TexStorage2D, &scr, "glTexStorage2D");
    load(gl.TexParameteri, &scr, "glTexParameteri");
    load(gl.DeleteTextures, &scr, "glDeleteTextures");
    load(gl.GetError, &scr, "glGetError");

    ygfx_gabor_desc gab;
    memset(&gab, 0, sizeof gab);
    gab.x = -300;
    gab.sf = 1 / 32.0f;
    gab.sigma = 40;
    gab.contrast = 0.5f;
    ygfx_stim g = ygfx_gabor(&gab);
    ygfx_shape_desc shd;
    memset(&shd, 0, sizeof shd);
    shd.shape = YGFX_RECT;
    shd.x = -300; shd.y = 250; shd.w = 200; shd.h = 60;
    shd.color[0] = 0.8f; shd.color[1] = 0.2f; shd.color[2] = 0.1f;
    ygfx_stim rect = ygfx_shape(&shd);

    ygfx_encoding srgb;
    memset(&srgb, 0, sizeof srgb);
    // filly writes sRGB-encoded RGBA8 by default; the gfx decodes to linear light.
    srgb.matrix = YGFX_MATRIX_RGB;
    srgb.range = YGFX_RANGE_FULL;
    srgb.transfer = YGFX_TRC_SRGB;
    srgb.primaries = YGFX_PRIM_DEVICE;

    // Shown at most 512 px wide in the window, whatever the target size.
    float show_w = float(std::min(tw, 512)), show_h = show_w * float(th) / float(tw);

    std::unique_ptr<filly::Renderer> renderer;
    std::optional<filly::Scene> scene;
    std::optional<filly::Model> model;
    std::optional<filly::ImportedTarget> imported;
    std::optional<filly::RenderTarget> offscreen;
    std::optional<filly::Readback> pending;
    std::vector<uint8_t> pixels;
    ygfx_tex ytex{0};
    GLuint host_tex = 0;
    bool animated = false;
    std::vector<ThreadInfo> threads_before = threads();

    if (shared || readback) {
        int64_t t0 = now_ns();
        if (shared) {
            // The host's texture: immutable RGBA8 in ysp's context, so Filament can make an
            // sRGB view of it and the gfx can import it.
            gl.GenTextures(1, &host_tex);
            gl.BindTexture(GL_TEXTURE_2D, host_tex);
            gl.TexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, tw, th);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            gl.BindTexture(GL_TEXTURE_2D, 0);
            ygfx_reset_state(&gfx);
            try {
                renderer = std::make_unique<filly::Renderer>(filly::current_gl_context());
            } catch (const std::exception& e) {
                fprintf(stderr, "filly shared renderer: %s\n", e.what());
                return 1;
            }
            imported = renderer->import_gl_target(host_tex, tw, th, "rgba8", true);
            ygfx_reset_state(&gfx);
            ygfx_import_desc id;
            memset(&id, 0, sizeof id);
            id.kind = YGFX_IMPORT_GL;
            id.w = tw; id.h = th;
            id.format = YGFX_RGBA8;
            id.enc = srgb;
            id.gl_tex[0] = host_tex;
            ytex = ygfx_texture_import(&gfx, &id);
            if (!ytex.id) { fprintf(stderr, "import: %s\n", ygfx_error(&gfx)); return 1; }
        } else {
            renderer = std::make_unique<filly::Renderer>();
            offscreen = renderer->create_render_target(tw, th, "rgba8", true);
            ygfx_texture_desc td;
            memset(&td, 0, sizeof td);
            td.w = tw; td.h = th; td.format = YGFX_RGBA8; td.linear = true; td.enc = srgb;
            ytex = ygfx_texture(&gfx, &td);
            if (!ytex.id) { fprintf(stderr, "texture: %s\n", ygfx_error(&gfx)); return 1; }
            pixels.resize(size_t(tw) * th * 4);
        }
        printf("filly: platform %s, Filament %s, renderer made in %.1f ms\n", renderer->gl_platform().c_str(),
               filly::filament_version(), (now_ns() - t0) / 1e6);
        scene = renderer->create_scene();
        std::vector<std::string> warnings;
        model = scene->load(model_path, false, warnings);
        for (auto& w : warnings) printf("  load warning: %s\n", w.c_str());
        animated = !model->animations().empty();
        filly::Camera cam = renderer->create_camera();
        filly::FrameOptions fo;
        fo.direction = filly::Vec3{-1, -0.3f, -1};
        fo.fit = fit;
        cam.frame(*model, fo);
        scene->set_camera(cam);
        scene->add_directional_light({-1, -1, -2}, 100000, {1, 1, 1});
        scene->set_background({0.2f, 0.2f, 0.25f, 1});
        // Compile before the first frame, so the timed loop does not include it.
        int64_t tp = now_ns();
        filly::Preparation prep = renderer->prepare(*scene);
        while (!prep.ready()) {
            if (shared) renderer->render(*scene, *imported);
            else renderer->render(*scene, *offscreen);
        }
        printf("filly: prepare %.1f ms, animated %d, %zu nodes\n", (now_ns() - tp) / 1e6, animated ? 1 : 0, model->nodes().size());
    }
    ygfx_prime(&gfx);

    // --- question 3: pixel checks ---------------------------------------------------
    if (check) {
        // The output with no filly calls at all, for the region outside the model.
        int W = win_w, H = win_h;
        std::vector<uint8_t> before(size_t(W) * H * 4), with_shared(size_t(W) * H * 4), with_cpu(size_t(W) * H * 4);
        auto frame = [&](ygfx_tex t, std::vector<uint8_t>& out, bool draw_model) {
            yscr_frame f;
            if (yscr_begin(&scr, &f) != YSCR_OK) return false;
            ygfx_begin(&gfx, &f);
            ygfx_draw(&gfx, &g);
            ygfx_draw(&gfx, &rect);
            if (draw_model) {
                ygfx_image_desc imd;
                memset(&imd, 0, sizeof imd);
                imd.tex = t; imd.x = 150; imd.w = show_w; imd.h = show_h; imd.linear = true;
                ygfx_stim im = ygfx_image(&gfx, &imd);
                ygfx_draw(&gfx, &im);
            }
            ygfx_end(&gfx);
            int rc = ygfx_read_output(&gfx, 0, 0, W, H, out.data());
            yscr_flip(&scr);
            return rc == YGFX_OK;
        };
        for (int i = 0; i < 5; i++) frame(ytex, before, false);
        if (!shared) { printf("check: --mode shared only\n"); return 0; }
        // A Filament-owned target in the same renderer gives the same pose's pixels by CPU.
        filly::RenderTarget ref = renderer->create_render_target(tw, th, "rgba8", true);
        ygfx_texture_desc td;
        memset(&td, 0, sizeof td);
        td.w = tw; td.h = th; td.format = YGFX_RGBA8; td.linear = true; td.enc = srgb;
        ygfx_tex cpu_tex = ygfx_texture(&gfx, &td);
        std::vector<uint8_t> top(size_t(tw) * th * 4), gl_order(top.size());
        int bad_outside = 0, bad_model = 0, max_diff = 0, checks = 0;
        for (int k = 0; k < 120; k++) {
            model->set_transform(rot_y(k * 3.0));
            if (animated) model->apply_animation(0, float(k) / 60.0f, true);
            renderer->render(*scene, *imported);
            renderer->render(*scene, ref);
            imported->acquire();
            frame(ytex, with_shared, true);
            imported->release();
            if (k % 10 != 9) continue;
            ref.read(top.data());
            // The shared texture holds GL's rows, bottom first, and the gfx samples texel row 0
            // as the top: upload the CPU copy in the same order so both show the same image.
            for (int y = 0; y < th; y++) memcpy(&gl_order[size_t(y) * tw * 4], &top[size_t(th - 1 - y) * tw * 4], size_t(tw) * 4);
            ygfx_texture_update(&gfx, cpu_tex, 0, 0, tw, th, gl_order.data(), 0);
            frame(cpu_tex, with_cpu, true);
            checks++;
            // Model rectangle in window pixels (rows top first).
            int mx0 = int(W / 2 + 150 - show_w / 2), my0 = int(H / 2 - show_h / 2);
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++) {
                    bool in_model = x >= mx0 && x < mx0 + int(show_w) && y >= my0 && y < my0 + int(show_h);
                    for (int c = 0; c < 3; c++) {
                        size_t o = (size_t(y) * W + x) * 4 + c;
                        int d1 = std::abs(int(with_shared[o]) - int(with_cpu[o]));
                        if (in_model) { if (d1) bad_model++; max_diff = std::max(max_diff, d1); }
                        else if (with_shared[o] != before[o]) bad_outside++;
                    }
                }
        }
        // For a person to look at: the last pose, through the shared texture.
        if (FILE* ppm = fopen("filly_check.ppm", "wb")) {
            fprintf(ppm, "P6 %d %d 255\n", W, H);
            for (size_t i = 0; i < size_t(W) * H; i++) fwrite(&with_shared[i * 4], 1, 3, ppm);
            fclose(ppm);
        }
        printf("check: %d pose(s); outside the model, channel values that differ from the output before filly: %d; "
               "inside it, shared minus CPU copy: %d differ, max %d\n", checks, bad_outside, bad_model, max_diff);
        imported.reset();
        renderer->close();
        ygfx_close(&gfx);
        yscr_close(&scr);
        return 0;
    }

    // --- question 2: the timed loop ------------------------------------------------------
    struct Row {
        int64_t index, onset_pred, render, wait, upload, gfx, flip, total;
        double gpu_ms;
    };
    std::vector<Row> rows;
    rows.reserve(size_t(frames));
    std::vector<yscr_record> recs;
    recs.reserve(size_t(frames) + 16);
    int64_t t_first = 0;
    std::vector<double> gpu;
    uint64_t last_gpu_frame = 0;
    yscr_frame f;
    double load0[3] = {0, 0, 0};
    getloadavg(load0, 3);
    while ((int64_t)rows.size() < frames && yscr_begin(&scr, &f) == YSCR_OK) {
        for (int i = 0; i < f.n_done; i++) recs.push_back(f.done[i]);
        if (f.index == 0) t_first = f.onset;
        Row r{};
        r.index = f.index;
        r.onset_pred = f.onset;
        int64_t c0 = now_ns(), c1 = c0, c2 = c0, c3 = c0;
        if (renderer) {
            // The pose follows the predicted onset: 90 degrees a second, and the clip's time.
            double t = double(f.onset - t_first) * 1e-9;
            model->set_transform(rot_y(90.0 * t));
            if (animated) model->apply_animation(0, float(t), true);
            if (shared) {
                renderer->render(*scene, *imported);
                c1 = now_ns();
                imported->acquire();
                c2 = c3 = now_ns();
            } else if (!async) {
                renderer->render(*scene, *offscreen);
                c1 = now_ns();
                offscreen->read(pixels.data());
                c2 = now_ns();
                ygfx_texture_update(&gfx, ytex, 0, 0, tw, th, pixels.data(), 0);
                c3 = now_ns();
            } else {
                renderer->render(*scene, *offscreen);
                filly::Readback next = offscreen->begin_read();
                c1 = now_ns();
                if (pending) {
                    pending->read(pixels.data());
                    c2 = now_ns();
                    ygfx_texture_update(&gfx, ytex, 0, 0, tw, th, pixels.data(), 0);
                }
                c3 = now_ns();
                pending = std::move(next);
            }
            yscr_mark(&scr, YSCR_PHASE_SCRIPT);
            filly::Stats st = renderer->stats();
            if (st.gpu_frame_ms && st.gpu_frame_number && *st.gpu_frame_number != last_gpu_frame) {
                last_gpu_frame = *st.gpu_frame_number;
                gpu.push_back(*st.gpu_frame_ms);
                r.gpu_ms = *st.gpu_frame_ms;
            }
        }
        ygfx_begin(&gfx, &f);
        ygfx_draw(&gfx, &g);
        ygfx_draw(&gfx, &rect);
        if (renderer) {
            ygfx_image_desc imd;
            memset(&imd, 0, sizeof imd);
            imd.tex = ytex; imd.x = 150; imd.w = show_w; imd.h = show_h; imd.linear = true;
            ygfx_stim im = ygfx_image(&gfx, &imd);
            ygfx_draw(&gfx, &im);
        }
        ygfx_end(&gfx);
        int64_t c4 = now_ns();
        // The host's fence after its last read of the texture: Filament waits on it before
        // it writes the next frame.
        if (shared) imported->release();
        yscr_flip(&scr);
        int64_t c5 = now_ns();
        r.render = c1 - c0; r.wait = c2 - c1; r.upload = c3 - c2; r.gfx = c4 - c3; r.flip = c5 - c4; r.total = c5 - c0;
        rows.push_back(r);
    }
    // Collect the last records.
    for (int i = 0; i < 6 && yscr_begin(&scr, &f) == YSCR_OK; i++) {
        for (int j = 0; j < f.n_done; j++) recs.push_back(f.done[j]);
        ygfx_begin(&gfx, &f);
        ygfx_end(&gfx);
        yscr_flip(&scr);
    }
    double load1[3] = {0, 0, 0};
    getloadavg(load1, 3);
    std::vector<ThreadInfo> threads_after = threads();

    // --- summary --------------------------------------------------------------------
    std::vector<double> ext, rend, wait, upl, gfxv, interval;
    for (auto& r : rows) {
        if (r.index < 10) continue;  // the first frames settle the queue
        ext.push_back((r.render + r.wait + r.upload) / 1e6);
        rend.push_back(r.render / 1e6);
        wait.push_back(r.wait / 1e6);
        upl.push_back(r.upload / 1e6);
        gfxv.push_back(r.gfx / 1e6);
    }
    std::sort(recs.begin(), recs.end(), [](const yscr_record& a, const yscr_record& b) { return a.index < b.index; });
    int64_t dropped = 0, late = 0, skipped = 0, n_rec = 0, paths[8] = {0};
    const yscr_record* prev = nullptr;
    for (auto& r : recs) {
        if (r.index < 10 || r.index >= (int64_t)rows.size()) continue;
        n_rec++;
        if (r.dropped) dropped++;
        if (r.flags & YSCR_FLIP_LATE_TARGET) late++;
        if (r.flags & (YSCR_FLIP_SKIPPED | YSCR_FLIP_CANCELED)) skipped++;
        if (r.path < 8) paths[r.path]++;
        if (prev && prev->index == r.index - 1 && r.onset && prev->onset) interval.push_back((r.onset - prev->onset) / 1e6);
        prev = &r;
    }
    auto show = [](const char* name, const Pct& p) {
        printf("  %-28s p50 %8.3f  p99 %8.3f  max %8.3f ms  (n %zu)\n", name, p.p50, p.p99, p.max, p.n);
    };
    printf("mode %s, target %dx%d, %zu frames, load average %.2f %.2f %.2f before, %.2f %.2f %.2f after\n", mode.c_str(),
           tw, th, rows.size(), load0[0], load0[1], load0[2], load1[0], load1[1], load1[2]);
    show("filly phase (CPU)", pct(ext));
    show("  render() submit", pct(rend));
    show(shared ? "  acquire() wait" : "  read() wait", pct(wait));
    show("  ygfx_texture_update", pct(upl));
    show("gfx begin..end (CPU)", pct(gfxv));
    show("filly GPU frame (Filament)", pct(gpu));
    show("flip interval (onsets)", pct(interval));
    printf("  records %" PRId64 ": dropped %" PRId64 ", late target %" PRId64 ", skipped %" PRId64 "; paths", n_rec, dropped, late, skipped);
    for (int i = 0; i < 8; i++) if (paths[i]) printf(" %d:%" PRId64, i, paths[i]);
    printf("\n");
    int over = 0;
    for (double v : interval) if (v > 1.5 * 16.667) over++;
    printf("  intervals over 1.5 periods: %d\n", over);
    printf("threads (tid name policy cpu-seconds over the run):\n");
    for (auto& t : threads_after) {
        double before = 0;
        bool existed = false;
        for (auto& b : threads_before) if (b.tid == t.tid) { before = b.cpu_s; existed = true; }
        printf("  %7d %-16s %s %7.2f%s\n", t.tid, t.name.c_str(),
               t.policy == SCHED_OTHER ? "OTHER" : t.policy == SCHED_FIFO ? "FIFO " : t.policy == SCHED_RR ? "RR   " :
               t.policy == SCHED_BATCH ? "BATCH" : t.policy == SCHED_IDLE ? "IDLE " : "?    ",
               t.cpu_s - before, existed ? "" : " (new)");
    }
    if (!csv.empty()) {
        if (FILE* out = fopen(csv.c_str(), "wb")) {
            fprintf(out, "index,onset_pred_ns,render_ns,wait_ns,upload_ns,gfx_ns,flip_ns,total_ns,gpu_ms\n");
            for (auto& r : rows)
                fprintf(out, "%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%" PRId64 ",%.4f\n",
                        r.index, r.onset_pred, r.render, r.wait, r.upload, r.gfx, r.flip, r.total, r.gpu_ms);
            fprintf(out, "#records index,onset_ns,dropped,flags,path,tier,script_ns,draw_ns,swap_ns\n");
            for (auto& r : recs)
                fprintf(out, "#r,%" PRId64 ",%" PRId64 ",%u,%u,%u,%u,%u,%u,%u\n", r.index, r.onset, r.dropped, r.flags, r.path, r.tier,
                        r.phase_ns[YSCR_PHASE_SCRIPT], r.phase_ns[YSCR_PHASE_DRAW], r.phase_ns[YSCR_PHASE_SWAP]);
            fclose(out);
        }
    }
    pending.reset();
    imported.reset();
    if (renderer) renderer->close();
    ygfx_close(&gfx);
    yscr_close(&scr);
    return 0;
}
