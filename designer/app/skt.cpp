// skt.cpp - fonts, the shared layout cache, the glyph atlas as ImGui user
// textures, and layouts drawn as quads (docs/designer_ui_probe.md, part 2).
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"   // RegisterUserTexture, ImTextureDataQueueUpload

#include "skribidi/skb_font_collection.h"
#include "skribidi/skb_image_atlas.h"
#include "skribidi/skb_layout.h"
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
ImVector<ImTextureData*> pages;   // one per atlas texture
int n_fonts = 0;

// --- the layout cache --------------------------------------------------------
//
// Skribidi's own cache (skb_layout_cache_compact()) drops an evicted entry
// without skb_layout_destroy(), so its memory is never freed: a scroll
// through 10,000 table rows kept 40 MB more on each pass (docs/designer.md,
// "The layout cache"). This one owns its layouts and frees what it evicts.
// An entry not used for kKeepFrames frames goes at the next
// skt_frame_begin(). The designer draws on input only, so a frame is a
// person's action, not 1/60 s.
const uint32_t kKeepFrames = 100;

struct Entry {
    uint64_t hash;
    skb_layout_t* layout;   // nullptr: on the free list
    char* text;             // the key's bytes: a hash collision is not a hit
    int32_t len;
    float size;
    int32_t prev, next;     // the LRU list, most recent first; next is also
                            // the free list's link
    uint32_t used;          // the frame it was last used in
};

ImVector<Entry> entries;
ImVector<int32_t> slots;      // open addressing, linear probing; -1 empty
int32_t lru_head = -1, lru_tail = -1, free_head = -1;
int live = 0;
uint32_t frame_no = 0;
uint64_t made = 0;

uint64_t key_hash(const char* s, int32_t n, float size) {
    uint64_t h = 14695981039346656037ull;   // FNV-1a
    uint32_t sb = 0;
    memcpy(&sb, &size, sizeof sb);
    for (int i = 0; i < 4; i++) h = (h ^ ((sb >> (8 * i)) & 0xffu)) * 1099511628211ull;
    for (int32_t i = 0; i < n; i++) h = (h ^ (uint8_t)s[i]) * 1099511628211ull;
    return h;
}

uint32_t slot_mask() { return (uint32_t)slots.Size - 1u; }

void lru_unlink(int32_t i) {
    Entry& e = entries[i];
    if (e.prev >= 0) entries[e.prev].next = e.next;
    else lru_head = e.next;
    if (e.next >= 0) entries[e.next].prev = e.prev;
    else lru_tail = e.prev;
    e.prev = e.next = -1;
}

void lru_push_front(int32_t i) {
    Entry& e = entries[i];
    e.prev = -1;
    e.next = lru_head;
    if (lru_head >= 0) entries[lru_head].prev = i;
    lru_head = i;
    if (lru_tail < 0) lru_tail = i;
}

void slots_insert(int32_t idx) {
    uint32_t i = (uint32_t)entries[idx].hash & slot_mask();
    while (slots[(int)i] >= 0) i = (i + 1u) & slot_mask();
    slots[(int)i] = idx;
}

void slots_grow() {
    const int n = slots.Size ? slots.Size * 2 : 1024;
    slots.resize(n);
    for (int i = 0; i < n; i++) slots[i] = -1;
    for (int32_t i = 0; i < entries.Size; i++)
        if (entries[i].layout) slots_insert(i);
}

// Removes entry idx's slot; later slots of its probe run move back, so a
// lookup never stops at a hole (no tombstones).
void slots_remove(int32_t idx) {
    const uint32_t m = slot_mask();
    uint32_t i = (uint32_t)entries[idx].hash & m;
    while (slots[(int)i] != idx) i = (i + 1u) & m;
    uint32_t j = i;
    for (;;) {
        j = (j + 1u) & m;
        const int32_t k = slots[(int)j];
        if (k < 0) break;
        const uint32_t home = (uint32_t)entries[k].hash & m;
        // k may fill the hole at i when its home is not in (i, j]
        if (((j - home) & m) >= ((j - i) & m)) {
            slots[(int)i] = k;
            i = j;
        }
    }
    slots[(int)i] = -1;
}

void evict(int32_t idx) {
    Entry& e = entries[idx];
    slots_remove(idx);
    lru_unlink(idx);
    skb_layout_destroy(e.layout);
    free(e.text);
    e.layout = nullptr;
    e.text = nullptr;
    e.next = free_head;
    free_head = idx;
    live--;
}

void cache_clear() {
    for (Entry& e : entries) {
        if (e.layout) skb_layout_destroy(e.layout);
        free(e.text);
    }
    entries.clear();
    slots.clear();
    lru_head = lru_tail = free_head = -1;
    live = 0;
}

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
    return true;
}

void skt_shutdown() {
    // ImGui rebuilds the renderer's texture list only at a frame's end, and
    // the renderer backend's shutdown walks it: a page deleted here and left
    // in it is read after its free (main.cpp shuts the UI down first).
    ImVector<ImTextureData*>& listed = ImGui::GetPlatformIO().Textures;
    for (ImTextureData* t : pages) {
        if (!t) continue;
        ImGui::UnregisterUserTexture(t);
        listed.find_erase(t);
        IM_DELETE(t);
    }
    pages.clear();
    cache_clear();
    if (raster) skb_rasterizer_destroy(raster);
    if (atlas) skb_image_atlas_destroy(atlas);
    if (temp) skb_temp_alloc_destroy(temp);
    if (fonts) skb_font_collection_destroy(fonts);
    raster = nullptr;
    atlas = nullptr;
    temp = nullptr;
    fonts = nullptr;
    n_fonts = 0;
}

int skt_font_count() { return n_fonts; }
skb_font_collection_t* skt_fonts() { return fonts; }
skb_temp_alloc_t* skt_temp() { return temp; }

const skb_layout_t* skt_layout_n(const char* utf8, int n, float size) {
    if (!fonts) return nullptr;
    const int32_t len = n < 0 ? (int32_t)strlen(utf8) : (int32_t)n;
    const uint64_t h = key_hash(utf8, len, size);
    if (slots.Size) {
        for (uint32_t i = (uint32_t)h & slot_mask();; i = (i + 1u) & slot_mask()) {
            const int32_t k = slots[(int)i];
            if (k < 0) break;
            Entry& e = entries[k];
            if (e.hash == h && e.len == len && e.size == size && memcmp(e.text, utf8, (size_t)len) == 0) {
                e.used = frame_no;
                if (lru_head != k) {
                    lru_unlink(k);
                    lru_push_front(k);
                }
                return e.layout;
            }
        }
    }
    const skb_attribute_t attrs[] = { skb_attribute_make_font_size(size) };
    skb_layout_params_t params = {};
    params.font_collection = fonts;
    params.layout_width = SKB_SIZE_AUTO;
    params.layout_height = SKB_SIZE_AUTO;
    skb_layout_t* l = skb_layout_create_utf8(temp, &params, utf8, len, SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(attrs));
    char* text = (char*)malloc((size_t)len + 1);
    if (!l || !text) {
        if (l) skb_layout_destroy(l);
        free(text);
        return nullptr;
    }
    memcpy(text, utf8, (size_t)len);
    text[len] = 0;
    // at most half full, so a probe run stays short
    if ((live + 1) * 2 > slots.Size) slots_grow();
    int32_t idx = -1;
    if (free_head >= 0) {
        idx = free_head;
        free_head = entries[idx].next;
    } else {
        idx = entries.Size;
        entries.push_back(Entry());
    }
    Entry& e = entries[idx];
    e.hash = h;
    e.layout = l;
    e.text = text;
    e.len = len;
    e.size = size;
    e.used = frame_no;
    e.prev = e.next = -1;
    lru_push_front(idx);
    slots_insert(idx);
    live++;
    made++;
    return l;
}

const skb_layout_t* skt_layout(const char* utf8, float size) { return skt_layout_n(utf8, -1, size); }

int skt_layout_count() { return live; }
uint64_t skt_layouts_made() { return made; }

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
    // The list's tail is the least recently used: stop at the first entry
    // still in use, so a frame pays only for what it evicts.
    frame_no++;
    while (lru_tail >= 0 && frame_no - entries[lru_tail].used > kKeepFrames) evict(lru_tail);
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
            const uint8_t* src = img->buffer + (size_t)yy * (size_t)img->stride_bytes + (size_t)r.x * (size_t)img->bpp;
            uint8_t* dst = (uint8_t*)t->GetPixelsAt(r.x, yy);
            for (int xx = 0; xx < r.width; xx++, dst += 4, src += img->bpp) {
                if (img->bpp == 1) { dst[0] = dst[1] = dst[2] = 255; dst[3] = src[0]; }
                else memcpy(dst, src, 4);
            }
        }
        ImTextureDataQueueUpload(t, r.x, r.y, r.width, r.height);
    }
}
