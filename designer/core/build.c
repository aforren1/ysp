/* build.c - Build: the pack tool on a thread of its own (dsg.h, step 6;
 * docs/designer.md, "Build"). OWNED BY PART B.
 *
 * Part S's stub: the builder starts and takes no job, so Build says that it
 * is not built yet; dsg_build_check_pack() is check.c's build call as it was
 * before step 6 (no entry cache). Part B replaces this file. */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "ysp/pack_tool.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct dsg_builder {
    dsg_builder_desc desc;
};

dsg_builder* dsg_builder_start(const dsg_builder_desc* d, char* err, size_t cap) {
    dsg_builder* b = (dsg_builder*)calloc(1, sizeof *b);
    if (!b) {
        snprintf(err, cap, "out of memory for the builder");
        return NULL;
    }
    if (d) b->desc = *d;
    return b;
}

void dsg_builder_stop(dsg_builder* b) { free(b); }

/* Decision 1: PROJECT/build/NAME.ysppak, NAME the folder's last name. */
int dsg_build_path(const dsg_project* p, char* out, size_t cap) {
    const char* f = p->folder;
    size_t n = strlen(f);
    const char* name;
    int k;
    while (n > 1 && (f[n - 1] == '/' || f[n - 1] == '\\')) n--;
    name = f + n;
    while (name > f && name[-1] != '/' && name[-1] != '\\') name--;
    if (name == f + n) return -1;
    k = snprintf(out, cap, "%.*s/%s/%.*s.ysppak", (int)n, f, DSG_BUILD_DIR, (int)(f + n - name), name);
    return k < 0 || (size_t)k >= cap ? -1 : 0;
}

int dsg_build_post(dsg_builder* b, const dsg_project* p, const char* out_path, uint64_t revision, char* err, size_t cap) {
    (void)b;
    (void)p;
    (void)out_path;
    (void)revision;
    snprintf(err, cap, "Build is not built yet (designer step 6, part B)");
    return -1;
}

void dsg_build_cancel(dsg_builder* b) { (void)b; }

bool dsg_build_busy(const dsg_builder* b) {
    (void)b;
    return false;
}

bool dsg_build_take(dsg_builder* b, const dsg_build_result** out) {
    (void)b;
    *out = NULL;
    return false;
}

int dsg_build_wait(dsg_builder* b, int64_t timeout_ns) {
    (void)b;
    (void)timeout_ns;
    return 0;
}

bool dsg_build_stale(const dsg_project* p, const dsg_build_result* r, uint64_t revision) {
    (void)p;
    return !r || r->status != DSG_BUILD_OK || r->revision != revision;
}

int dsg_build_check_pack(const dsg_project* p, const char* desc, size_t desc_len, const char* out_path, char* err, size_t cap) {
    ypt_options o;
    memset(&o, 0, sizeof o);
    return ypt_build_text(desc, desc_len, p->folder, out_path, &o, err, cap) == 0 ? 0 : -1;
}
