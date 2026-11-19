/* chart.c - the chart model and its commands (chart.h). OWNED BY PART A.
 * A stub: every function refuses until part A builds it. */
#include "chart.h"

#include <string.h>

int dsg_chart_build(const yjs_value* root, const char* tmpl, dsg_chart* out) {
    (void)root;
    memset(out, 0, sizeof *out);
    (void)tmpl;
    return -1;
}

void dsg_chart_free(dsg_chart* c) { memset(c, 0, sizeof *c); }

int dsg_chart_add_state(dsg_doc* d, const char* tmpl, const char* name) { (void)d; (void)tmpl; (void)name; return -1; }
int dsg_chart_connect(dsg_doc* d, const char* tmpl, int from, const char* to) { (void)d; (void)tmpl; (void)from; (void)to; return -1; }
int dsg_chart_delete_state(dsg_doc* d, const char* tmpl, int state) { (void)d; (void)tmpl; (void)state; return -1; }
int dsg_chart_delete_exit(dsg_doc* d, const char* exit_path) { (void)d; (void)exit_path; return -1; }
int dsg_chart_rename_state(dsg_doc* d, const char* tmpl, int state, const char* name) { (void)d; (void)tmpl; (void)state; (void)name; return -1; }
int dsg_chart_set_first(dsg_doc* d, const char* tmpl, int state) { (void)d; (void)tmpl; (void)state; return -1; }
