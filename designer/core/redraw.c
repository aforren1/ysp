/* redraw.c - when the designer draws (docs/designer.md, "Redraw rule").
 * Idle, it draws nothing and blocks in the event wait: 0.00 % of a core in
 * the probe, against 10.8 % drawing every frame (designer_ui_probe.md). */
#include "dsg.h"

void dsg_redraw_event(dsg_redraw* r) { r->settle = DSG_SETTLE_FRAMES; }

bool dsg_redraw_due(const dsg_redraw* r, int queued, bool animating) {
    /* ImGui applies one key or button change a frame: a burst of input
     * needs as many frames as it has changes, whatever the settle count. */
    return r->settle > 0 || queued > 0 || animating || r->work;
}

void dsg_redraw_drawn(dsg_redraw* r) {
    if (r->settle > 0) r->settle--;
}

int dsg_redraw_wait_ms(const dsg_redraw* r, int queued, bool animating) {
    return dsg_redraw_due(r, queued, animating) ? 0 : -1;
}
