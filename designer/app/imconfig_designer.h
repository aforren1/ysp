// imconfig_designer.h - Dear ImGui's configuration for the designer.
#pragma once
// 32-bit indices: GL ES 3.0 and WebGL2 have no base-vertex draw, so with
// 16-bit ones a window's draw list stops at 65,536 vertices; a definition's
// text drawn one quad a glyph passes that at about 16,000 glyphs.
#define ImDrawIdx unsigned int
#define IMGUI_DISABLE_OBSOLETE_FUNCTIONS
// Characters above U+FFFF (emoji, some CJK) reach the Skribidi editor whole.
#define IMGUI_USE_WCHAR32
