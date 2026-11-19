// views.cpp - the list of views (views.h). Shared: a new view adds one
// line here; the step 4 views are here already.
#include "views.h"

extern const UiView ui_view_chart;    // view_chart.cpp, step 4 part A
extern const UiView ui_view_tables;   // view_tables.cpp, step 4 part B
extern const UiView ui_view_panels;   // view_panels.cpp, step 4 part C

const UiView* const* ui_views(int* n) {
    static const UiView* const list[] = { &ui_view_chart, &ui_view_tables, &ui_view_panels };
    *n = (int)(sizeof list / sizeof list[0]);
    return list;
}
