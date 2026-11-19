// view_tables.h - what the tables' tests read from the grid editor
// (view_tables.cpp; step 4, part B). OWNED BY PART B. The designer itself
// reaches the view only through views.h.
#pragma once
#include "imgui.h"

struct ImGuiWindow;
struct SkfField;

// Cell texts laid out in the last frame that drew the grid: only the
// visible rows' and columns' cells, and the header's.
int view_tables_layouts_last_frame();
// The data rows the last frame drew, first and last; -1 when none.
void view_tables_rows_last_frame(int* first, int* last);
// The item of a cell in the last frame's grid: row -1 is the header, col -1
// the row numbers. 0 when no grid was drawn.
ImGuiID view_tables_cell_id(int row, int col);
// The grid's scrolling window, or NULL; its ImGui table's ID, or 0.
ImGuiWindow* view_tables_grid_window();
ImGuiID view_tables_table_id();
// The width of column name in the open table, px; 0 when it has none.
float view_tables_width(const char* name);
// The edited cell's field, or NULL when no cell is edited.
SkfField* view_tables_field();
