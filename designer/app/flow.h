// flow.h - rows of items that wrap (docs/designer.md, "Rows that wrap
// (2026-11-23)"). ImGui's SameLine() does not wrap: an item past the
// window's right edge is clipped. In the hand test of 2026-11-23 the
// preview's Pop out, last of 11 items on one row, was never seen unless the
// window was full screen.
//
// Before each item after the first of a row, call flow_next() with the
// item's width: the item goes on this row when it fits in what is left,
// else it starts the next row. The widths are ImGui's own for each kind of
// item. A group that must stay on one row (a label and its field) passes
// the group's width once, then SameLine() between its parts.
#pragma once
#include "imgui.h"
#include "imgui_internal.h"

// Text in ImGui's font, as TextUnformatted() and Text() give it.
inline float flow_text_w(const char* s) { return ImGui::CalcTextSize(s).x; }

// A label's text: a "##" hides the rest.
inline float flow_label_w(const char* label) { return ImGui::CalcTextSize(label, nullptr, true).x; }

// Button() and SmallButton(): the label and the frame's padding.
inline float flow_button_w(const char* label) { return flow_label_w(label) + 2.0f * ImGui::GetStyle().FramePadding.x; }

// Checkbox() and RadioButton(): the box, a frame high, then the label.
inline float flow_check_w(const char* label) {
    const float t = flow_label_w(label);
    return ImGui::GetFrameHeight() + (t > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.0f);
}

// A combo or a field of width w (SetNextItemWidth()), then its label.
inline float flow_field_w(float w, const char* label) {
    const float t = flow_label_w(label);
    return w + (t > 0.0f ? ImGui::GetStyle().ItemInnerSpacing.x + t : 0.0f);
}

// What is left of the current row after the last item and the spacing.
inline float flow_left() {
    const ImGuiWindow* win = ImGui::GetCurrentWindow();
    const float right = ImGui::GetCursorScreenPos().x + ImGui::GetContentRegionAvail().x;
    return right - (win->DC.CursorPosPrevLine.x + ImGui::GetStyle().ItemSpacing.x);
}

// Before an item of width w that follows another: SameLine() when it fits,
// else nothing, so it starts the next row. True when it stays on the row.
inline bool flow_next(float w) {
    if (w > flow_left()) return false;
    ImGui::SameLine();
    return true;
}
