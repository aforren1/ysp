// views.cpp - the list of views (views.h). Shared: a new view adds one
// line here; the step 4 views and the step 5 preview are here already.
#include "views.h"

extern const UiView ui_view_chart;    // view_chart.cpp, step 4 part A
extern const UiView ui_view_tables;   // view_tables.cpp, step 4 part B
extern const UiView ui_view_panels;   // view_panels.cpp, step 4 part C
extern const UiView ui_view_preview;  // view_preview.cpp, step 5 part B

const UiView* const* ui_views(int* n) {
    static const UiView* const list[] = { &ui_view_chart, &ui_view_tables, &ui_view_panels, &ui_view_preview };
    *n = (int)(sizeof list / sizeof list[0]);
    return list;
}
