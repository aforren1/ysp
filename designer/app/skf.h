// skf.h - a Skribidi edit field as an ImGui widget (skf.cpp): user text is
// edited with skb_editor, never with ImGui's InputText (docs/designer.md,
// "User text").
#pragma once
#include "imgui.h"

struct SkfField;

// multiline: Enter starts a paragraph and the field grows with its text, for
// the caller to put in a scrolling child window; else one line that scrolls
// inside its box.
SkfField* skf_create(float font_size, bool multiline);
void skf_destroy(SkfField* f);
// Replaces the text; not a change in skf_draw()'s sense, and not undoable.
void skf_set_text(SkfField* f, const char* utf8, int len);
// The text as UTF-8, NUL-ended, in out; its length in bytes.
int skf_text(const SkfField* f, ImVector<char>* out);
// Draws the field and handles its input; true when its text changed
// (typing, a paste, an undo, a committed composition). width or height <= 0:
// the window's available space.
bool skf_draw(const char* id, SkfField* f, float width, float height);

// The caret as a codepoint offset into the text.
int skf_caret(const SkfField* f);
// Puts the caret before codepoint cp, gives the field the focus, and scrolls
// the caret into view at the next skf_draw().
void skf_set_caret(SkfField* f, int cp);
bool skf_focused(const SkfField* f);
// The caret as skf_set_caret() puts it, without taking the focus: the text
// view follows an undo that another view started.
void skf_move_caret(SkfField* f, int cp);
// own: the field's Ctrl+Z, Ctrl+Y and Ctrl+Shift+Z go to Skribidi's undo
// (the default). The definition editor sets false: the document's one undo
// stack takes those keys (docs/designer.md, "Plan of step 4").
void skf_set_own_undo(SkfField* f, bool own);
bool skf_owns_undo(const SkfField* f);
// The field that takes text now, or NULL.
SkfField* skf_focus_field();

// Input that does not come through ImGui's queue, for the focused field:
// the IME's composition (SDL_EVENT_TEXT_EDITING natively; the web glue's
// composition events later), and a browser's clipboard events.
void skf_compose(const char* utf8, int caret_cp);
void skf_paste(const char* utf8);
