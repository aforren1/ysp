// skt.h - user text through Skribidi in ImGui (docs/designer.md, "User
// text"). Skribidi lays out and shapes; its rasterizer fills its image atlas;
// each atlas page is an ImGui user texture; a glyph is one textured quad.
// ImGui's own font draws only the designer's own UI text.
#pragma once
#include <stddef.h>
#include <stdint.h>

struct ImDrawList;
typedef struct skb_layout_t skb_layout_t;
typedef struct skb_rich_layout_t skb_rich_layout_t;
typedef struct skb_font_collection_t skb_font_collection_t;
typedef struct skb_temp_alloc_t skb_temp_alloc_t;

// Loads the fonts that font_dir/fonts.txt lists, one file name a line, in
// fallback order (CMake writes the list). False with a reason when none
// loads.
bool skt_init(const char* font_dir, char* err, size_t err_cap);
void skt_shutdown();
int skt_font_count();
skb_font_collection_t* skt_fonts();
skb_temp_alloc_t* skt_temp();

// A one-line layout of UTF-8 text at `size` px from the shared layout cache
// (keyed by text and size; a hit costs a hash and a lookup).
const skb_layout_t* skt_layout(const char* utf8, float size);

// Glyph quads for a layout at (x, y), layout units times `scale`. Glyphs
// outside the draw list's clip rectangle are skipped before the atlas sees
// them.
void skt_draw_layout(ImDrawList* dl, const skb_layout_t* layout, float x, float y, float scale, uint32_t col);
void skt_draw_rich_layout(ImDrawList* dl, const skb_rich_layout_t* layout, float x, float y, uint32_t col);

// Once a frame: before the panels (frees atlas items no longer drawn) and
// after them, before ImGui::Render() (rasterizes new glyphs, queues uploads).
void skt_frame_begin();
void skt_frame_end();
