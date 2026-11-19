// skt.cpp - fonts, the shared layout cache, the glyph atlas as ImGui user
// textures, and layouts drawn as quads (docs/designer_ui_probe.md, part 2).
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture, ImTextureDataQueueUpload

#include "skribidi/skb_font_collection.h"
#include "skribidi/skb_image_atlas.h"
#include "skribidi/skb_layout.h"
#include "skribidi/skb_layout_cache.h"
#include "skribidi/skb_rasterizer.h"
#include "skribidi/skb_rich_layout.h"

// skb_common.h tests __cplusplus__ (sic), so in C++ too its SKB_LITERAL()
// makes a C compound literal, which MSVC refuses (C4576). As a C++20
// designated initializer it is the same value.
#undef SKB_LITERAL
#define SKB_LITERAL(type) type

#include "dsg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {

skb_font_collection_t* fonts = nullptr;
skb_temp_alloc_t* temp = nullptr;
skb_image_atlas_t* atlas = nullptr;
skb_rasterizer_t* raster = nullptr;
skb_layout_cache_t* cache = nullptr;
ImVector<ImTextureData*> pages;   // one per atlas texture
int n_fonts = 0;

ImU32 to_im(skb_color_t c) { return IM_COL32(c.r, c.g, c.b, c.a); }

ImTextureData* page(int idx) {
    while (pages.Size <= idx) pages.push_back(nullptr);
    if (!pages[idx]) {
        const skb_image_t* img = skb_image_atlas_get_texture(atlas, idx);
        ImTextureData* t = IM_NEW(ImTextureData)();
        t->Create(ImTextureFormat_RGBA32, img->width, img->height);
        memset(t->Pixels, 0, (size_t)t->GetSizeInBytes());
        ImGui::RegisterUserTexture(t);
        pages[idx] = t;
    }
    return pages[idx];
}

void quad(ImDrawList* dl, const skb_quad_t& q, float ox, float oy, float s) {
    if (q.flags & SKB_QUAD_IS_EMPTY) return;
    ImTextureData* t = page(q.texture_idx);
    const float iw = 1.0f / (float)t->Width, ih = 1.0f / (float)t->Height;
    ImVec2 p0(ox + q.geom.x * s, oy + q.geom.y * s);
    ImVec2 p1(p0.x + q.geom.width * s, p0.y + q.geom.height * s);
    // pattern maps the geometry onto the texture rectangle; a glyph uses all of it
    ImVec2 uv0((q.texture.x + q.pattern.x * q.texture.width) * iw, (q.texture.y + q.pattern.y * q.texture.height) * ih);
    ImVec2 uv1((q.texture.x + (q.pattern.x + q.pattern.width) * q.texture.width) * iw,
               (q.texture.y + (q.pattern.y + q.pattern.height) * q.texture.height) * ih);
    dl->AddImage(t->GetTexRef(), p0, p1, uv0, uv1, to_im(q.color));
}

} // namespace

bool skt_init(const char* font_dir, char* err, size_t err_cap) {
    char path[DSG_PATH_MAX];
    char* list = nullptr;
    size_t n = 0;
    if (dsg_join(path, sizeof path, font_dir, "fonts.txt") || dsg_read_file(path, &list, &n, err, err_cap)) return false;
    fonts = skb_font_collection_create();
    // The names that fail and why, for the error: a CI run on Windows gave
    // only "no font loads", with files that the configure step had found.
    char tried[512] = "";
    size_t tried_len = 0;
    int n_lines = 0;
    for (char* line = list; line && *line;) {
        char* end = strchr(line, '\n');
        if (end) *end = 0;
        size_t ll = strlen(line);
        if (ll && line[ll - 1] == '\r') line[--ll] = 0;
        if (*line) {
            n_lines++;
            const char* why = nullptr;
            if (dsg_join(path, sizeof path, font_dir, line)) why = "path too long";
            else if (skb_font_collection_add_font(fonts, path, SKB_FONT_FAMILY_DEFAULT, nullptr)) n_fonts++;
            else {
                FILE* f = fopen(path, "rb");
                why = f ? "not loaded" : "cannot open";
                if (f) fclose(f);
            }
            if (why && tried_len + 1 < sizeof tried) {
                int w = snprintf(tried + tried_len, sizeof tried - tried_len, "%s%s (%s)", tried_len ? ", " : "", line, why);
                if (w > 0) tried_len += (size_t)w < sizeof tried - tried_len ? (size_t)w : sizeof tried - tried_len - 1;
            }
        }
        line = end ? end + 1 : nullptr;
    }
    free(list);
    if (n_fonts == 0) {
        snprintf(err, err_cap, "no font in %s/fonts.txt loads: %d listed%s%s", font_dir, n_lines, n_lines ? ": " : "", tried);
        return false;
    }
    temp = skb_temp_alloc_create(512 * 1024);
    // One fixed page size: a full page starts another, so a page never
    // changes size under an ImGui texture.
    skb_image_atlas_config_t cfg = skb_image_atlas_get_default_config();
    cfg.init_width = cfg.max_width = 1024;
    cfg.init_height = cfg.max_height = 1024;
    atlas = skb_image_atlas_create(&cfg);
    raster = skb_rasterizer_create(nullptr);
    cache = skb_layout_cache_create();
    return true;
}

void skt_shutdown() {
    for (ImTextureData* t : pages) {
        if (!t) continue;
        ImGui::UnregisterUserTexture(t);
        IM_DELETE(t);
    }
    pages.clear();
    if (cache) skb_layout_cache_destroy(cache);
    if (raster) skb_rasterizer_destroy(raster);
    if (atlas) skb_image_atlas_destroy(atlas);
    if (temp) skb_temp_alloc_destroy(temp);
    if (fonts) skb_font_collection_destroy(fonts);
    cache = nullptr;
    raster = nullptr;
    atlas = nullptr;
    temp = nullptr;
    fonts = nullptr;
    n_fonts = 0;
}

int skt_font_count() { return n_fonts; }
skb_font_collection_t* skt_fonts() { return fonts; }
skb_temp_alloc_t* skt_temp() { return temp; }

const skb_layout_t* skt_layout(const char* utf8, float size) {
    const skb_attribute_t attrs[] = { skb_attribute_make_font_size(size) };
    skb_layout_params_t params = {};
    params.font_collection = fonts;
    params.layout_width = SKB_SIZE_AUTO;
    params.layout_height = SKB_SIZE_AUTO;
    return skb_layout_cache_get_utf8(cache, temp, &params, utf8, -1, SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(attrs));
}

void skt_draw_layout(ImDrawList* dl, const skb_layout_t* layout, float x, float y, float scale, uint32_t col) {
    const skb_layout_params_t* lp = skb_layout_get_params(layout);
    const skb_layout_line_t* lines = skb_layout_get_lines(layout);
    const int nl = skb_layout_get_lines_count(layout);
    const skb_layout_run_t* runs = skb_layout_get_layout_runs(layout);
    const skb_glyph_t* glyphs = skb_layout_get_glyphs(layout);
    const skb_color_t tint = { (uint8_t)(col & 0xff), (uint8_t)((col >> 8) & 0xff), (uint8_t)((col >> 16) & 0xff),
                               (uint8_t)(col >> 24) };
    const ImVec4 clip = dl->_ClipRectStack.back();
    for (int li = 0; li < nl; li++) {
        const skb_layout_line_t* line = &lines[li];
        // a whole line outside the clip: a long definition draws only what shows
        if (y + (line->bounds.y + line->bounds.height) * scale < clip.y || y + line->bounds.y * scale > clip.w) continue;
        for (int32_t ri = line->layout_run_range.start; ri < line->layout_run_range.end; ri++) {
            const skb_layout_run_t* run = &runs[ri];
            if (run->type != SKB_CONTENT_RUN_UTF8 && run->type != SKB_CONTENT_RUN_UTF32) continue;
            for (int32_t gi = run->glyph_range.start; gi < run->glyph_range.end; gi++) {
                const skb_glyph_t* g = &glyphs[gi];
                const float gx = x + g->offset_x * scale, gy = y + g->offset_y * scale;
                if (gx > clip.z || gx < clip.x - 4 * run->font_size * scale || gy < clip.y || gy - 2 * run->font_size * scale > clip.w)
                    continue;
                skb_quad_t q = skb_image_atlas_get_glyph_quad(atlas, g->offset_x, g->offset_y, scale, lp->font_collection,
                                                              run->font_handle, g->gid, run->font_size, tint, SKB_RASTERIZE_ALPHA_MASK);
                quad(dl, q, x, y, scale);
            }
        }
    }
}

void skt_draw_rich_layout(ImDrawList* dl, const skb_rich_layout_t* rl, float x, float y, uint32_t col) {
    for (int32_t pi = 0; pi < skb_rich_layout_get_paragraphs_count(rl); pi++) {
        const skb_vec2_t o = skb_rich_layout_get_layout_offset(rl, pi);
        skt_draw_layout(dl, skb_rich_layout_get_layout(rl, pi), x + o.x, y + o.y, 1.0f, col);
    }
}

void skt_frame_begin() {
    if (atlas) skb_image_atlas_compact(atlas);
}

void skt_frame_end() {
    if (!atlas || !skb_image_atlas_rasterize_missing_items(atlas, temp, raster)) return;
    for (int i = 0; i < skb_image_atlas_get_texture_count(atlas); i++) {
        skb_rect2i_t r = skb_image_atlas_get_and_reset_texture_dirty_bounds(atlas, i);
        if (skb_rect2i_is_empty(r)) continue;
        const skb_image_t* img = skb_image_atlas_get_texture(atlas, i);
        ImTextureData* t = page(i);
        // ImGui's GL backend takes RGBA only (WebGL2 has no texture swizzle),
        // so coverage goes to alpha under white and the quad's color tints it.
        for (int yy = r.y; yy < r.y + r.height; yy++) {
            const uint8_t* src = img->buffer + (size_t)yy * img->stride_bytes + (size_t)r.x * img->bpp;
            uint8_t* dst = (uint8_t*)t->GetPixelsAt(r.x, yy);
            for (int xx = 0; xx < r.width; xx++, dst += 4, src += img->bpp) {
                if (img->bpp == 1) { dst[0] = dst[1] = dst[2] = 255; dst[3] = src[0]; }
                else memcpy(dst, src, 4);
            }
        }
        ImTextureDataQueueUpload(t, r.x, r.y, r.width, r.height);
    }
}
