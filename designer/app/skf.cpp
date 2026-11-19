// skf.cpp - a Skribidi edit field as an ImGui widget (docs/designer_ui_probe.md,
// part 2, cleaned up from the spike). skb_editor owns the text, the caret,
// the selection, undo and the IME composition; this file maps ImGui's mouse,
// keys and characters onto it, draws it with skt.cpp, and tells the platform
// where the caret is, for the IME's window.
#include "skf.h"
#include "skt.h"

#include "imgui_internal.h"   // ItemAdd, SetActiveID, PlatformImeData

#include "skribidi/skb_editor.h"
#include "skribidi/skb_rich_layout.h"

#include <string.h>

struct SkfField {
    skb_editor_t* ed = nullptr;
    bool multiline = false;
    bool focused = false;
    bool composing = false;
    bool changed = false;      // set by the editor's text callback
    bool reveal = false;       // scroll the caret into view at the next draw
    bool own_undo = true;      // Ctrl+Z is Skribidi's, not the document's
    float size = 16.0f;
};

namespace {

const float kPad = 6.0f;      // inner margin
SkfField* focus = nullptr;     // the field that takes text, IME and clipboard

void on_change(skb_editor_t*, skb_editor_text_change_reason_t, void* ctx) { ((SkfField*)ctx)->changed = true; }

// UTF-8 to codepoints; invalid bytes become U+FFFD one by one.
void decode(const char* s, int n, ImVector<uint32_t>* out) {
    out->resize(0);
    const unsigned char* p = (const unsigned char*)s;
    const unsigned char* e = p + n;
    while (p < e) {
        uint32_t c = *p;
        int k = c < 0x80 ? 0 : (c >> 5) == 6 ? 1 : (c >> 4) == 14 ? 2 : (c >> 3) == 30 ? 3 : -1;
        if (k < 0 || e - p <= k) { out->push_back(0xFFFD); p++; continue; }
        c = k == 0 ? c : k == 1 ? (c & 0x1Fu) : k == 2 ? (c & 0x0Fu) : (c & 0x07u);
        bool ok = true;
        for (int i = 1; i <= k; i++) {
            if (p + i >= e || (p[i] & 0xC0) != 0x80) { ok = false; break; }
            c = (c << 6) | (p[i] & 0x3Fu);
        }
        if (!ok) { out->push_back(0xFFFD); p++; continue; }
        out->push_back(c);
        p += k + 1;
    }
}

void selection_utf8(const SkfField* f, ImVector<char>* out) {
    int n = skb_editor_get_text_utf8_count_in_range(f->ed, SKB_CURRENT_SELECTION);
    out->resize(n + 1);
    skb_editor_get_text_utf8_in_range(f->ed, SKB_CURRENT_SELECTION, out->Data, n + 1);
    out->Data[n] = 0;
}

struct SelDraw { ImDrawList* dl; ImVec2 o; };
void sel_rect(skb_rect2_t r, void* ctx) {
    SelDraw* s = (SelDraw*)ctx;
    s->dl->AddRectFilled(ImVec2(s->o.x + r.x, s->o.y + r.y), ImVec2(s->o.x + r.x + r.width, s->o.y + r.y + r.height),
                         IM_COL32(60, 100, 170, 255));
}

void insert(SkfField* f, const uint32_t* cps, int n) {
    if (f->composing) {
        skb_editor_commit_composition_utf32(f->ed, skt_temp(), cps, n);
        f->composing = false;
    } else {
        skb_editor_insert_text_utf32(f->ed, skt_temp(), SKB_CURRENT_SELECTION, cps, n);
    }
}

void keys(SkfField* f, uint32_t mods) {
    ImGuiIO& io = ImGui::GetIO();
    static const struct { ImGuiKey k; skb_editor_key_t s; } map[] = {
        { ImGuiKey_LeftArrow, SKB_KEY_LEFT }, { ImGuiKey_RightArrow, SKB_KEY_RIGHT },
        { ImGuiKey_UpArrow, SKB_KEY_UP },     { ImGuiKey_DownArrow, SKB_KEY_DOWN },
        { ImGuiKey_Home, SKB_KEY_HOME },      { ImGuiKey_End, SKB_KEY_END },
        { ImGuiKey_Backspace, SKB_KEY_BACKSPACE }, { ImGuiKey_Delete, SKB_KEY_DELETE },
    };
    for (const auto& k : map)
        if (ImGui::IsKeyPressed(k.k, true)) { skb_editor_process_key_pressed(f->ed, skt_temp(), k.s, mods); f->reveal = true; }
    if (f->multiline && (ImGui::IsKeyPressed(ImGuiKey_Enter, true) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, true))) {
        skb_editor_process_key_pressed(f->ed, skt_temp(), SKB_KEY_ENTER, mods);
        f->reveal = true;
    }
    // JSON is indented with spaces; a tab character would not survive the
    // canonical writer anyway.
    if (f->multiline && ImGui::IsKeyPressed(ImGuiKey_Tab, true) && !io.KeyCtrl) {
        static const uint32_t two[2] = { ' ', ' ' };
        insert(f, two, 2);
        f->reveal = true;
    }
    if (io.KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_A, false)) skb_editor_select_all(f->ed);
        if (ImGui::IsKeyPressed(ImGuiKey_C, false) || ImGui::IsKeyPressed(ImGuiKey_X, false)) {
            ImVector<char> s;
            selection_utf8(f, &s);
            if (s.Size > 1) {
                ImGui::SetClipboardText(s.Data);
                if (ImGui::IsKeyPressed(ImGuiKey_X, false)) skb_editor_remove(f->ed, skt_temp(), SKB_CURRENT_SELECTION);
            }
        }
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) {
            const char* s = ImGui::GetClipboardText();
            if (s && *s) skb_editor_insert_text_utf8(f->ed, skt_temp(), SKB_CURRENT_SELECTION, s, -1);
            f->reveal = true;
        }
        if (f->own_undo && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
            if (io.KeyShift) skb_editor_redo(f->ed, skt_temp());
            else skb_editor_undo(f->ed, skt_temp());
            f->reveal = true;
        }
        if (f->own_undo && ImGui::IsKeyPressed(ImGuiKey_Y, false)) { skb_editor_redo(f->ed, skt_temp()); f->reveal = true; }
    }
    // Characters, in the order ImGui's queue kept them with the keys.
    if (!io.InputQueueCharacters.empty()) {
        ImVector<uint32_t> cps;
        for (ImWchar c : io.InputQueueCharacters)
            if (c >= 0x20 && c != 0x7F) cps.push_back((uint32_t)c);
        io.InputQueueCharacters.resize(0);
        if (!cps.empty()) {
            insert(f, cps.Data, cps.Size);
            f->reveal = true;
        }
    }
}

} // namespace

SkfField* skf_create(float font_size, bool multiline) {
    SkfField* f = IM_NEW(SkfField)();
    f->size = font_size;
    f->multiline = multiline;
    static const skb_attribute_t line_attrs[] = {
        skb_attribute_make_text_overflow(SKB_OVERFLOW_SCROLL),
        skb_attribute_make_vertical_align(SKB_ALIGN_CENTER),
    };
    const skb_attribute_t text_attrs[] = { skb_attribute_make_font_size(font_size) };
    // A color for the composition; skt_draw_layout() tints every glyph alike,
    // so it does not show yet (probe part 2, section 6).
    static const skb_attribute_t comp_attrs[] = {
        skb_attribute_make_paint_color(SKB_PAINT_TEXT, SKB_PAINT_STATE_DEFAULT, skb_rgba(120, 200, 255, 255)),
    };
    skb_editor_params_t p = {};
    p.font_collection = skt_fonts();
    if (!multiline) p.layout_attributes = SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(line_attrs);
    p.paragraph_attributes = SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(text_attrs);
    p.composition_attributes = SKB_ATTRIBUTE_SET_FROM_STATIC_ARRAY(comp_attrs);
    // a multi-line field is as large as its text: its window scrolls it
    p.editor_width = multiline ? SKB_SIZE_AUTO : 200.0f;
    p.editor_height = multiline ? SKB_SIZE_AUTO : font_size * 1.6f;
    f->ed = skb_editor_create(&p);
    /* A new editor has no paragraph: its caret has height 0 and an IME
     * composition does not show (docs/layout.md, Known issues). An empty
     * text gives it one; set before the callback so it is not an edit. */
    skb_editor_set_text_utf8(f->ed, skt_temp(), "", 0);
    skb_editor_set_on_text_change_callback(f->ed, on_change, f);
    return f;
}

void skf_destroy(SkfField* f) {
    if (!f) return;
    if (focus == f) focus = nullptr;
    skb_editor_destroy(f->ed);
    IM_DELETE(f);
}

void skf_set_text(SkfField* f, const char* utf8, int len) {
    skb_editor_set_text_utf8(f->ed, skt_temp(), utf8, len);
    f->composing = false;
    f->changed = false;
}

int skf_text(const SkfField* f, ImVector<char>* out) {
    int n = skb_editor_get_text_utf8_count(f->ed);
    out->resize(n + 1);
    skb_editor_get_text_utf8(f->ed, out->Data, n + 1);
    out->Data[n] = 0;
    return n;
}

int skf_caret(const SkfField* f) {
    skb_text_range_t sel = skb_editor_get_current_selection(f->ed);
    return skb_editor_get_text_offset_from_text_position(f->ed, sel.end);
}

void skf_set_caret(SkfField* f, int cp) {
    // {cp, trailing} is the caret before codepoint cp, as the editor's own
    // moves place it (tests/layout/editor_test.c)
    skb_text_range_t r;
    r.start.offset = r.end.offset = cp;
    r.start.affinity = r.end.affinity = SKB_AFFINITY_TRAILING;
    skb_editor_select(f->ed, r);
    f->focused = true;
    focus = f;
    f->reveal = true;
}

bool skf_focused(const SkfField* f) { return f->focused; }

void skf_move_caret(SkfField* f, int cp) {
    skb_text_range_t r;
    r.start.offset = r.end.offset = cp;
    r.start.affinity = r.end.affinity = SKB_AFFINITY_TRAILING;
    skb_editor_select(f->ed, r);
    f->reveal = true;
}

void skf_set_own_undo(SkfField* f, bool own) { f->own_undo = own; }
bool skf_owns_undo(const SkfField* f) { return f->own_undo; }
SkfField* skf_focus_field() { return focus; }

void skf_compose(const char* utf8, int caret_cp) {
    if (!focus) return;
    ImVector<uint32_t> cps;
    decode(utf8, (int)strlen(utf8), &cps);
    if (cps.empty()) {
        skb_editor_clear_composition(focus->ed, skt_temp());
        focus->composing = false;
    } else {
        skb_editor_set_composition_utf32(focus->ed, skt_temp(), cps.Data, cps.Size, caret_cp);
        focus->composing = true;
    }
    focus->reveal = true;
}

void skf_paste(const char* utf8) {
    if (!focus) return;
    skb_editor_insert_text_utf8(focus->ed, skt_temp(), SKB_CURRENT_SELECTION, utf8, -1);
    focus->reveal = true;
}

bool skf_draw(const char* id_label, SkfField* f, float width, float height) {
    ImGuiContext& g = *ImGui::GetCurrentContext();
    ImGuiWindow* win = ImGui::GetCurrentWindow();
    ImGuiIO& io = ImGui::GetIO();
    const ImGuiID id = win->GetID(id_label);
    const ImVec2 avail = ImGui::GetContentRegionAvail();
    if (width <= 0) width = avail.x;
    if (height <= 0) height = avail.y;
    if (f->multiline) {
        const skb_rect2_t lb = skb_editor_get_layout_bounds(f->ed);
        width = ImMax(width, lb.x + lb.width + 2 * kPad + f->size);
        height = ImMax(height, lb.y + lb.height + 2 * kPad + f->size);
    }
    const ImVec2 o = win->DC.CursorPos;
    const ImRect bb(o, ImVec2(o.x + width, o.y + height));
    ImGui::ItemSize(bb);
    if (!ImGui::ItemAdd(bb, id)) return false;
    const bool hovered = ImGui::ItemHoverable(bb, id, 0);
    if (hovered) ImGui::SetMouseCursor(ImGuiMouseCursor_TextInput);
    const ImVec2 to(o.x + kPad, o.y + (f->multiline ? kPad : 0));
    const ImVec2 m = io.MousePos;
    const uint32_t mods = (io.KeyShift ? SKB_MOD_SHIFT : 0) | (io.KeyCtrl ? SKB_MOD_CONTROL : 0) | (io.KeyAlt ? SKB_MOD_ALT : 0);

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (hovered) {
            f->focused = true;
            focus = f;
            ImGui::SetActiveID(id, win);
            ImGui::SetFocusID(id, win);
            const skb_vec2_t vo = skb_editor_get_view_offset(f->ed);
            // Skribidi counts double and triple clicks from these times.
            skb_editor_process_mouse_click(f->ed, m.x - to.x - vo.x, m.y - to.y - vo.y, mods, ImGui::GetTime());
        } else if (f->focused && !ImGui::IsAnyItemHovered()) {
            // a click on empty space; a click on a button keeps the focus, so
            // Save or a jump to an issue does not take the caret away
            f->focused = false;
            if (focus == f) focus = nullptr;
        }
    } else if (g.ActiveId == id && ImGui::IsMouseDown(ImGuiMouseButton_Left) && (io.MouseDelta.x != 0 || io.MouseDelta.y != 0)) {
        const skb_vec2_t vo = skb_editor_get_view_offset(f->ed);
        skb_editor_process_mouse_drag(f->ed, m.x - to.x - vo.x, m.y - to.y - vo.y);
    }
    if (g.ActiveId == id && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) ImGui::ClearActiveID();
    // An ImGui text field took the keyboard: both would read the characters.
    if (f->focused && g.ActiveId != 0 && g.ActiveId != id && g.InputTextState.ID == g.ActiveId) {
        f->focused = false;
        if (focus == f) focus = nullptr;
    }
    if (f->focused) keys(f, mods);

    ImDrawList* dl = win->DrawList;
    dl->AddRectFilled(bb.Min, bb.Max, IM_COL32(25, 27, 32, 255), 3);
    if (!f->multiline) dl->AddRect(bb.Min, bb.Max, f->focused ? IM_COL32(255, 200, 80, 255) : IM_COL32(90, 95, 110, 255), 3);
    dl->PushClipRect(bb.Min, bb.Max, true);
    const skb_vec2_t vo = skb_editor_get_view_offset(f->ed);
    const ImVec2 tv(to.x + vo.x, to.y + vo.y);
    const skb_text_range_t sel = skb_editor_get_current_selection(f->ed);
    const bool has_sel = skb_editor_get_text_range_count(f->ed, sel) > 0;
    if (has_sel) {
        SelDraw sd = { dl, tv };
        skb_editor_iterate_text_range_bounds(f->ed, sel, sel_rect, &sd);
    }
    skt_draw_rich_layout(dl, skb_editor_get_rich_layout(f->ed), tv.x, tv.y, IM_COL32(235, 235, 240, 255));
    const skb_caret_info_t c = skb_editor_get_caret_info_at(f->ed, SKB_CURRENT_SELECTION_END);
    if (f->focused && !has_sel) {
        const float x = tv.x + c.x, y = tv.y + c.y;
        dl->AddLine(ImVec2(x + c.ascender * c.slope, y + c.ascender), ImVec2(x + c.descender * c.slope, y + c.descender),
                    IM_COL32(255, 200, 80, 255), 2.0f);
        // a tick on the caret's top shows which way the text runs there
        const float d = skb_is_rtl((skb_text_direction_t)c.direction) ? -4.0f : 4.0f;
        dl->AddLine(ImVec2(x, y + c.ascender), ImVec2(x + d, y + c.ascender), IM_COL32(255, 200, 80, 255), 2.0f);
    }
    dl->PopClipRect();

    if (f->reveal && f->multiline) {
        // the caret into the window's view, with a line of margin
        const ImRect view = win->InnerClipRect;
        const float top = tv.y + c.y + c.ascender - f->size, bottom = tv.y + c.y + c.descender + f->size;
        const float left = tv.x + c.x - 2 * f->size, right = tv.x + c.x + 2 * f->size;
        if (top < view.Min.y) ImGui::SetScrollY(win, win->Scroll.y - (view.Min.y - top));
        else if (bottom > view.Max.y) ImGui::SetScrollY(win, win->Scroll.y + (bottom - view.Max.y));
        if (left < view.Min.x) ImGui::SetScrollX(win, ImMax(0.0f, win->Scroll.x - (view.Min.x - left)));
        else if (right > view.Max.x) ImGui::SetScrollX(win, win->Scroll.x + (right - view.Max.x));
    }
    f->reveal = false;

    if (f->focused) {
        // The platform's text input and the IME's window follow the caret.
        g.PlatformImeData.WantVisible = true;
        g.PlatformImeData.WantTextInput = true;
        g.PlatformImeData.InputPos = ImVec2(tv.x + c.x, tv.y + c.y + c.ascender);
        g.PlatformImeData.InputLineHeight = c.descender - c.ascender;
        g.PlatformImeData.ViewportId = win->Viewport->ID;
    }
    const bool changed = f->changed;
    f->changed = false;
    return changed;
}
