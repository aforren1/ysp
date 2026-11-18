/* filly as a ysp Renderer extension in the browser (docs/filly_probe.md,
 * question 4). ysp's web build (SDL3, WebGL2, ASYNCIFY) owns the canvas and
 * its context. filly's web build is a second Emscripten module on the same
 * page; it registers ysp's WebGL2RenderingContext with its own GL layer and
 * renders into a texture that ysp made. The page glue is index.html.
 *
 *   --mode none|filly   the loop without filly, or with it
 *   --frames N          timed frames (default 3600)
 *   --size WxH          filly's target (default 512x512)
 *   --check             question 3: pixel checks instead of a timing run
 *   --no-reset          skip ygfx_reset_state() after filly's calls
 */
#define YSP_GFX_IMPLEMENTATION
#include "ysp/gfx.h"

#include <emscripten.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

EM_JS_DEPS(filly_web_deps, "$GL,$UTF8ToString,$stringToUTF8,$HEAPU8");

EM_JS(double, web_perf_ms, (void), { return performance.now(); })
EM_JS(double, web_raf_t_ms, (void), { return Module['yscrRafT'] || 0; })
/* ysp's context and texture object go to the page, which makes filly's renderer on them. */
EM_ASYNC_JS(int, probe_setup, (int tex_name, int w, int h), {
    const gl = GL.currentContext.GLctx;
    return await Module['fillyProbe'].setup(gl, GL.textures[tex_name], w, h);
})
/* One filly frame for the pose at t seconds, then filly hands the context back. */
EM_JS(double, probe_frame, (double t), { return Module['fillyProbe'].frame(t); })
EM_JS(double, probe_gpu_ms, (void), { return Module['fillyProbe'].gpuMs(); })
/* The target's pixels as GL stores them (bottom row first), into dst. */
EM_JS(void, probe_read, (uint8_t* dst), { HEAPU8.set(Module['fillyProbe'].readGlOrder(), dst); })
EM_JS(void, probe_result, (const char* json), { Module['fillyProbe'].result(UTF8ToString(json)); })
EM_JS(int, probe_renderer_string, (char* dst, int cap), {
    const gl = GL.currentContext.GLctx;
    const ext = gl.getExtension('WEBGL_debug_renderer_info');
    const s = ext ? gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) : gl.getParameter(gl.RENDERER);
    stringToUTF8(s, dst, cap);
    return 0;
})

static yscr_screen scr;
static ygfx_gfx gfx;

#define MAX_FRAMES 8192
static double t_filly[MAX_FRAMES], t_gfx[MAX_FRAMES], t_iv[MAX_FRAMES], t_gpu[MAX_FRAMES];

static int cmp(const void* a, const void* b) {
    double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}
static void pct(double* v, int n, double out[3]) {
    out[0] = out[1] = out[2] = 0;
    if (n <= 0) return;
    qsort(v, (size_t)n, sizeof *v, cmp);
    out[0] = v[(int)(0.5 * (n - 1) + 0.5)];
    out[1] = v[(int)(0.99 * (n - 1) + 0.5)];
    out[2] = v[n - 1];
}

typedef void (*gen_fn)(int, unsigned*);
typedef void (*bind_fn)(unsigned, unsigned);
typedef void (*storage_fn)(unsigned, int, unsigned, int, int);
typedef void (*param_fn)(unsigned, unsigned, int);

static void draw_frame(const yscr_frame* f, const ygfx_stim* g, const ygfx_stim* rect, ygfx_tex t, float sw, float sh) {
    ygfx_begin(&gfx, f);
    ygfx_draw(&gfx, g);
    ygfx_draw(&gfx, rect);
    if (t.id) {
        ygfx_image_desc imd;
        ygfx_stim im;
        memset(&imd, 0, sizeof imd);
        imd.tex = t; imd.x = 150; imd.w = sw; imd.h = sh; imd.linear = true;
        im = ygfx_image(&gfx, &imd);
        ygfx_draw(&gfx, &im);
    }
    ygfx_end(&gfx);
}

int main(int argc, char** argv) {
    int i, tw = 512, th = 512, filly = 1, check = 0, reset = 1, frames = 3600, W = 1024, H = 768;
    yscr_desc sd;
    ygfx_desc gd;
    yscr_frame f;
    ygfx_gabor_desc gab;
    ygfx_shape_desc shd;
    ygfx_encoding srgb;
    ygfx_tex ytex = { 0 };
    unsigned tex = 0;
    char line[1024], renderer[256], json[2048];
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--mode") && i + 1 < argc) filly = strcmp(argv[++i], "none") != 0;
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &tw, &th);
        else if (!strcmp(argv[i], "--check")) check = 1;
        else if (!strcmp(argv[i], "--no-reset")) reset = 0;
    }
    if (frames > MAX_FRAMES) frames = MAX_FRAMES;
    memset(&sd, 0, sizeof sd);
    sd.windowed = true;
    sd.window_w = W;
    sd.window_h = H;
    if (!yscr_open(&scr, &sd)) { fprintf(stderr, "screen: %s\n", yscr_error(&scr)); return 1; }
    memset(&gd, 0, sizeof gd);
    gd.screen = &scr;
    gd.background[0] = gd.background[1] = gd.background[2] = 0.5f;
    if (!ygfx_open(&gfx, &gd)) { fprintf(stderr, "gfx: %s\n", ygfx_error(&gfx)); return 1; }
    yscr_describe(&scr, line, sizeof line);
    printf("screen: %s\n", line);
    probe_renderer_string(renderer, sizeof renderer);
    printf("webgl renderer: %s\n", renderer);

    memset(&gab, 0, sizeof gab);
    gab.x = -300; gab.sf = 1 / 32.0f; gab.sigma = 40; gab.contrast = 0.5f;
    ygfx_stim g = ygfx_gabor(&gab);
    memset(&shd, 0, sizeof shd);
    shd.shape = YGFX_RECT;
    shd.x = -300; shd.y = 250; shd.w = 200; shd.h = 60;
    shd.color[0] = 0.8f; shd.color[1] = 0.2f; shd.color[2] = 0.1f;
    ygfx_stim rect = ygfx_shape(&shd);
    memset(&srgb, 0, sizeof srgb);
    srgb.matrix = YGFX_MATRIX_RGB; srgb.range = YGFX_RANGE_FULL; srgb.transfer = YGFX_TRC_SRGB; srgb.primaries = YGFX_PRIM_DEVICE;
    float show_w = (float)(tw < 512 ? tw : 512), show_h = show_w * (float)th / (float)tw;

    if (filly) {
        gen_fn gen = (gen_fn)yscr_gl_proc(&scr, "glGenTextures");
        bind_fn bind = (bind_fn)yscr_gl_proc(&scr, "glBindTexture");
        storage_fn storage = (storage_fn)yscr_gl_proc(&scr, "glTexStorage2D");
        param_fn param = (param_fn)yscr_gl_proc(&scr, "glTexParameteri");
        ygfx_import_desc id;
        double t0 = web_perf_ms();
        gen(1, &tex);
        bind(0x0DE1, tex);                       /* GL_TEXTURE_2D */
        storage(0x0DE1, 1, 0x8058, tw, th);      /* GL_RGBA8 */
        param(0x0DE1, 0x2801, 0x2601);           /* MIN_FILTER LINEAR */
        param(0x0DE1, 0x2800, 0x2601);           /* MAG_FILTER LINEAR */
        bind(0x0DE1, 0);
        if (probe_setup((int)tex, tw, th) != 0) { fprintf(stderr, "filly setup failed\n"); return 1; }
        ygfx_reset_state(&gfx);
        memset(&id, 0, sizeof id);
        id.kind = YGFX_IMPORT_GL; id.w = tw; id.h = th; id.format = YGFX_RGBA8; id.enc = srgb; id.gl_tex[0] = tex;
        ytex = ygfx_texture_import(&gfx, &id);
        if (!ytex.id) { fprintf(stderr, "import: %s\n", ygfx_error(&gfx)); return 1; }
        printf("filly: setup (renderer, model, prepare) %.1f ms\n", web_perf_ms() - t0);
    }
    ygfx_prime(&gfx);

    if (check) {
        size_t n = (size_t)W * H * 4;
        uint8_t* before = (uint8_t*)malloc(n);
        uint8_t* with_shared = (uint8_t*)malloc(n);
        uint8_t* with_cpu = (uint8_t*)malloc(n);
        uint8_t* px = (uint8_t*)malloc((size_t)tw * th * 4);
        ygfx_texture_desc td;
        ygfx_tex cpu_tex;
        long bad_outside = 0, bad_model = 0, max_diff = 0, checks = 0, k;
        for (i = 0; i < 5; i++) {
            if (yscr_begin(&scr, &f) != YSCR_OK) return 1;
            draw_frame(&f, &g, &rect, (ygfx_tex){ 0 }, show_w, show_h);
            ygfx_read_output(&gfx, 0, 0, W, H, before);
            yscr_flip(&scr);
        }
        memset(&td, 0, sizeof td);
        td.w = tw; td.h = th; td.format = YGFX_RGBA8; td.linear = true; td.enc = srgb;
        cpu_tex = ygfx_texture(&gfx, &td);
        for (k = 0; k < 120; k++) {
            int rc;
            if (yscr_begin(&scr, &f) != YSCR_OK) return 1;
            probe_frame((double)k / 30.0);
            if (reset) ygfx_reset_state(&gfx);
            draw_frame(&f, &g, &rect, ytex, show_w, show_h);
            rc = ygfx_read_output(&gfx, 0, 0, W, H, with_shared);
            yscr_flip(&scr);
            if (rc != YGFX_OK) printf("read_output: %d\n", rc);
            if (k % 10 != 9) continue;
            probe_read(px);
            if (reset) ygfx_reset_state(&gfx);
            ygfx_texture_update(&gfx, cpu_tex, 0, 0, tw, th, px, 0);
            if (yscr_begin(&scr, &f) != YSCR_OK) return 1;
            draw_frame(&f, &g, &rect, cpu_tex, show_w, show_h);
            ygfx_read_output(&gfx, 0, 0, W, H, with_cpu);
            yscr_flip(&scr);
            checks++;
            {
                int mx0 = (int)(W / 2 + 150 - show_w / 2), my0 = (int)(H / 2 - show_h / 2), x, y, c;
                for (y = 0; y < H; y++)
                    for (x = 0; x < W; x++) {
                        int in_model = x >= mx0 && x < mx0 + (int)show_w && y >= my0 && y < my0 + (int)show_h;
                        for (c = 0; c < 3; c++) {
                            size_t o = ((size_t)y * W + x) * 4 + c;
                            int d = abs((int)with_shared[o] - (int)with_cpu[o]);
                            if (in_model) { if (d) bad_model++; if (d > max_diff) max_diff = d; }
                            else if (with_shared[o] != before[o]) bad_outside++;
                        }
                    }
            }
        }
        snprintf(json, sizeof json, "{\"kind\":\"check\",\"reset\":%d,\"renderer\":\"%s\",\"checks\":%ld,\"bad_outside\":%ld,"
                 "\"bad_model\":%ld,\"max_diff\":%ld}", reset, renderer, checks, bad_outside, bad_model, max_diff);
        printf("%s\n", json);
        probe_result(json);
        return 0;
    }

    {
        int n = 0, n_iv = 0, n_gpu = 0;
        long dropped = 0, late = 0, records = 0;
        double prev_raf = 0, p_f[3], p_g[3], p_iv[3], p_gpu[3], prev_gpu = -1;
        int64_t t_first = 0;
        long over = 0;
        while (n < frames && yscr_begin(&scr, &f) == YSCR_OK) {
            double a, b, c, t = web_raf_t_ms();
            for (i = 0; i < f.n_done; i++) {
                const yscr_record* r = &f.done[i];
                if (r->index < 10) continue;
                records++;
                if (r->dropped) dropped++;
                if (r->flags & YSCR_FLIP_LATE_TARGET) late++;
            }
            if (f.index == 0) t_first = f.onset;
            if (prev_raf > 0 && t > prev_raf && f.index >= 10) {
                t_iv[n_iv++] = t - prev_raf;
                if (t - prev_raf > 1.5 * 16.667) over++;
            }
            prev_raf = t;
            a = web_perf_ms();
            if (filly) {
                double gpu;
                probe_frame((double)(f.onset - t_first) * 1e-9);
                yscr_mark(&scr, YSCR_PHASE_SCRIPT);
                if (reset) ygfx_reset_state(&gfx);
                gpu = probe_gpu_ms();
                if (gpu >= 0 && gpu != prev_gpu && f.index >= 10) t_gpu[n_gpu++] = gpu;
                prev_gpu = gpu;
            }
            b = web_perf_ms();
            draw_frame(&f, &g, &rect, ytex, show_w, show_h);
            c = web_perf_ms();
            yscr_flip(&scr);
            if (f.index >= 10) { t_filly[n] = b - a; t_gfx[n] = c - b; n++; } else frames++;
        }
        pct(t_filly, n, p_f);
        pct(t_gfx, n, p_g);
        pct(t_iv, n_iv, p_iv);
        pct(t_gpu, n_gpu, p_gpu);
        snprintf(json, sizeof json,
                 "{\"kind\":\"timing\",\"mode\":\"%s\",\"reset\":%d,\"size\":\"%dx%d\",\"renderer\":\"%s\",\"frames\":%d,"
                 "\"filly_ms\":[%.4f,%.4f,%.4f],\"gfx_ms\":[%.4f,%.4f,%.4f],\"raf_interval_ms\":[%.4f,%.4f,%.4f],"
                 "\"intervals_over_1_5\":%ld,\"gpu_ms\":[%.4f,%.4f,%.4f],\"gpu_n\":%d,\"records\":%ld,\"dropped\":%ld,\"late\":%ld}",
                 filly ? "filly" : "none", reset, tw, th, renderer, n, p_f[0], p_f[1], p_f[2], p_g[0], p_g[1], p_g[2],
                 p_iv[0], p_iv[1], p_iv[2], over, p_gpu[0], p_gpu[1], p_gpu[2], n_gpu, records, dropped, late);
        printf("%s\n", json);
        probe_result(json);
    }
    return 0;
}
