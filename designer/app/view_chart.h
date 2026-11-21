// view_chart.h - the chart's seam to the preview (designer step 5, part C;
// docs/designer.md, "Part C as built"). OWNED BY PART C.
//
// The chart reads the preview's place and pins a template only through
// these hooks. The tests put in a test double: it gives each place at
// once, with no session to load and step, and it works on S's stub of
// preview.c, which runs no session.
#pragma once
#include <stdint.h>
#include "preview.h"

struct ChartPreviewHooks {
    const dsg_preview_where* (*where)();             // ui_preview_where()
    bool (*pin)(dsg_preview* pv, const char* tmpl);  // dsg_preview_pin_template()
};

// NULL, or a hook left NULL, puts back the real call.
void ui_chart_set_preview_hooks(const ChartPreviewHooks* h);
// The ring's color (IM_COL32), so a test can find the ring in the draw list.
uint32_t ui_chart_ring_color();
