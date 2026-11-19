/* project.c - a project folder: open, save and create, and the view state
 * (docs/designer.md, "Project folder"). */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#include "dsg.h"
#include "player.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char* err, size_t cap, const char* fmt, ...) {
    va_list ap;
    if (err && cap) {
        va_start(ap, fmt);
        vsnprintf(err, cap, fmt, ap);
        va_end(ap);
    }
    return -1;
}

int dsg_join(char* out, size_t cap, const char* folder, const char* rel) {
    size_t a = strlen(folder), b = strlen(rel), s = a && folder[a - 1] != '/' ? 1 : 0;
    if (a + s + b + 1 > cap) return -1;
    memcpy(out, folder, a);
    if (s) out[a] = '/';
    memcpy(out + a + s, rel, b + 1);
    return 0;
}
#define join dsg_join

static char* dup_bytes(const char* s, size_t n) {
    char* d = (char*)malloc(n + 1);
    if (!d) return NULL;
    if (n) memcpy(d, s, n);
    d[n] = 0;
    return d;
}

/* --- view state ------------------------------------------------------------ */

void dsg_view_free(dsg_view* v) {
    free(v->imgui_ini);
    memset(v, 0, sizeof *v);
}

int dsg_view_read(const char* folder, dsg_view* v, char* err, size_t cap) {
    char path[DSG_PATH_MAX];
    char* text = NULL;
    size_t n = 0;
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value* x;
    int rc = 0;
    memset(v, 0, sizeof *v);
    if (join(path, sizeof path, folder, DSG_VIEW_FILE)) return fail(err, cap, "the project's path is too long");
    if (!dsg_is_file(path)) return 0;
    if (dsg_read_file(path, &text, &n, err, cap)) return -1;
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, text, n, NULL, &root, &e) != YJS_OK) {
        rc = fail(err, cap, "%s: line %u, column %u: %s; the view starts empty", DSG_VIEW_FILE, e.line, e.column, e.msg);
    } else if (root->type != YJS_OBJECT || !(x = yjs_get(root, "format")) || x->type != YJS_STRING ||
               strcmp(x->u.s, "ysp-designer-view") != 0) {
        rc = fail(err, cap, "%s: not a ysp-designer-view file; the view starts empty", DSG_VIEW_FILE);
    } else {
        /* Every key is optional and one of a wrong type is skipped: a view
         * file from another designer version costs the view, not the project. */
        int64_t l = 0, c = 0;
        double s = 0;
        x = yjs_get(root, "caret");
        if (x && x->type == YJS_ARRAY && x->n == 2 && yjs_int64(x->u.items[0], &l) == YJS_OK &&
            yjs_int64(x->u.items[1], &c) == YJS_OK && l > 0 && c > 0 && l < UINT32_MAX && c < UINT32_MAX) {
            v->caret_line = (uint32_t)l;
            v->caret_column = (uint32_t)c;
        }
        if (yjs_double(yjs_get(root, "scroll_y"), &s) == YJS_OK && s >= 0 && s < 1e9) v->scroll_y = (float)s;
        x = yjs_get(root, "imgui");
        if (x && x->type == YJS_STRING) {
            v->imgui_ini = dup_bytes(x->u.s, x->n);
            v->imgui_ini_len = v->imgui_ini ? x->n : 0;
        }
    }
    yjs_arena_free(&a);
    free(text);
    return rc;
}

int dsg_view_write(const char* folder, const dsg_view* v, char* err, size_t cap) {
    char dir[DSG_PATH_MAX], path[DSG_PATH_MAX];
    yjs_arena a;
    yjs_value* root;
    char* buf;
    size_t n;
    int rc;
    if (join(dir, sizeof dir, folder, DSG_DIR) || join(path, sizeof path, folder, DSG_VIEW_FILE))
        return fail(err, cap, "the project's path is too long");
    if (dsg_make_dir(dir, err, cap)) return -1;
    yjs_arena_init_heap(&a, 0);
    root = yjs_new(&a, YJS_OBJECT);
    rc = !root;
    if (!rc) rc |= yjs_set(&a, root, "format", yjs_new_stringz(&a, "ysp-designer-view"));
    if (!rc) rc |= yjs_set(&a, root, "version", yjs_new_int(&a, 1));
    if (!rc && v->caret_line) {
        yjs_value* c = yjs_new(&a, YJS_ARRAY);
        rc |= !c || yjs_push(&a, c, yjs_new_int(&a, v->caret_line)) || yjs_push(&a, c, yjs_new_int(&a, v->caret_column)) ||
              yjs_set(&a, root, "caret", c);
    }
    if (!rc) rc |= yjs_set(&a, root, "scroll_y", yjs_new_double(&a, (double)v->scroll_y));
    if (!rc && v->imgui_ini && v->imgui_ini_len) rc |= yjs_set(&a, root, "imgui", yjs_new_string(&a, v->imgui_ini, v->imgui_ini_len));
    if (rc) { yjs_arena_free(&a); return fail(err, cap, "the view state: out of memory"); }
    n = yjs_write_mem(root, YJS_WRITE_PRETTY, NULL, 0);
    buf = (char*)malloc(n + 2);
    if (!buf) { yjs_arena_free(&a); return fail(err, cap, "the view state: out of memory"); }
    yjs_write_mem(root, YJS_WRITE_PRETTY, buf, n + 1);
    buf[n++] = '\n';
    yjs_arena_free(&a);
    rc = dsg_write_file(path, buf, n, err, cap);
    free(buf);
    return rc;
}

/* --- open ------------------------------------------------------------------ */

void dsg_project_close(dsg_project* p) {
    free(p->desc);
    free(p->saved);
    free(p->text);
    dsg_view_free(&p->view);
    memset(p, 0, sizeof *p);
}

/* The loader's reading of a definition at open: its version, and the
 * migrated bytes of a version 1 file (D12: only the designer migrates). */
static void read_version(dsg_project* p) {
    yjs_arena a;
    yexp_def* d = NULL;
    yexp_report* r = (yexp_report*)malloc(sizeof *r);
    if (!r) return;
    yjs_arena_init_heap(&a, 0);
    if (yexp_load(&a, p->saved, p->saved_len, NULL, &d, r) == YEXP_OK && d) {
        p->version = d->format_version;
        if (d->format_version == 1) {
            const char* bytes;
            size_t n;
            if (yexp_migrated(d, &a, &bytes, &n, NULL) == YEXP_OK) {
                char* m = dup_bytes(bytes, n);
                if (m) {
                    free(p->text);
                    p->text = m;
                    p->text_len = n;
                    p->migrated = true;
                }
            }
        }
    }
    yjs_arena_free(&a);
    free(r);
}

int dsg_project_open(dsg_project* p, const char* path, char* err, size_t cap) {
    yjs_arena a;
    yjs_value* root = NULL;
    yjs_error e;
    const yjs_value *fmt, *ex, *src;
    char verr[256];
    size_t len;
    memset(p, 0, sizeof *p);
    len = strlen(path);
    while (len > 1 && (path[len - 1] == '/' || path[len - 1] == '\\')) len--;
    if (len >= DSG_PATH_MAX - 64) return fail(err, cap, "'%s': the path is too long", path);
    if (dsg_is_dir(path)) {
        memcpy(p->folder, path, len);
        p->folder[len] = 0;
        join(p->desc_path, sizeof p->desc_path, p->folder, DSG_DESC_FILE);
    } else if (dsg_is_file(path)) {
        const char* s1 = strrchr(path, '/');
        const char* s2 = strrchr(path, '\\');
        const char* s = s1 > s2 ? s1 : s2;
        memcpy(p->desc_path, path, len);
        p->desc_path[len] = 0;
        if (s) {
            memcpy(p->folder, path, (size_t)(s - path));
            p->folder[s - path] = 0;
            if (!p->folder[0]) strcpy(p->folder, "/");
        } else {
            strcpy(p->folder, ".");
        }
    } else {
        return fail(err, cap, "'%s': no such folder or file", path);
    }
    if (dsg_read_file(p->desc_path, &p->desc, &p->desc_len, err, cap)) {
        if (!dsg_is_file(p->desc_path))
            fail(err, cap, "'%s' has no %s: a project is a folder with a pack source description", p->folder, DSG_DESC_FILE);
        dsg_project_close(p);
        return -1;
    }
    yjs_arena_init_heap(&a, 0);
    if (yjs_parse(&a, p->desc, p->desc_len, NULL, &root, &e) != YJS_OK) {
        fail(err, cap, "%s: line %u, column %u: %s", p->desc_path, e.line, e.column, e.msg);
        goto bad;
    }
    fmt = root->type == YJS_OBJECT ? yjs_get(root, "format") : NULL;
    if (!fmt || fmt->type != YJS_STRING || strcmp(fmt->u.s, "ysp-pack-source") != 0) {
        fail(err, cap, "%s: not a pack source description (\"format\": \"ysp-pack-source\")", p->desc_path);
        goto bad;
    }
    ex = yjs_get(root, "experiment");
    src = ex && ex->type == YJS_OBJECT ? yjs_get(ex, "source") : NULL;
    if (!src || src->type != YJS_STRING) {
        fail(err, cap, "%s: names no experiment (\"experiment\": {\"source\": ...})", p->desc_path);
        goto bad;
    }
    /* the pack tool's rule for a source, so the check pack can be built */
    if (!ypak_name_ok(src->u.s, src->n) || strchr(src->u.s, ':') || src->n >= sizeof p->def_source) {
        fail(err, cap, "%s: the experiment's source '%s' is not a relative path inside the folder", p->desc_path, src->u.s);
        goto bad;
    }
    memcpy(p->def_source, src->u.s, src->n + 1);
    yjs_arena_free(&a);
    if (join(p->def_path, sizeof p->def_path, p->folder, p->def_source)) {
        fail(err, cap, "'%s': the path is too long", p->def_source);
        dsg_project_close(p);
        return -1;
    }
    if (dsg_read_file(p->def_path, &p->saved, &p->saved_len, err, cap)) {
        dsg_project_close(p);
        return -1;
    }
    p->text = dup_bytes(p->saved, p->saved_len);
    if (!p->text) {
        dsg_project_close(p);
        return fail(err, cap, "out of memory");
    }
    p->text_len = p->saved_len;
    read_version(p);
    if (p->migrated)
        snprintf(p->note, sizeof p->note, "%.200s is format version 1; it is shown migrated to version 2, which Save writes (format 3.2)",
                 p->def_source);
    if (dsg_view_read(p->folder, &p->view, verr, sizeof verr))
        snprintf(p->note + strlen(p->note), sizeof p->note - strlen(p->note), "%s%s", p->note[0] ? ". " : "", verr);
    return 0;
bad:
    yjs_arena_free(&a);
    dsg_project_close(p);
    return -1;
}

bool dsg_project_dirty(const dsg_project* p, const char* text, size_t n) {
    return n != p->saved_len || (n && memcmp(text, p->saved, n) != 0);
}

/* --- save ------------------------------------------------------------------ */

void dsg_saved_free(dsg_saved* s) {
    free(s->bytes);
    memset(s, 0, sizeof *s);
}

int dsg_project_save(dsg_project* p, const char* text, size_t n, dsg_saved* out, char* err, size_t cap) {
    yjs_arena a;
    yexp_def* d = NULL;
    yexp_report* r = (yexp_report*)malloc(sizeof *r);
    const char* bytes = text;
    size_t len = n;
    char* copy;
    memset(out, 0, sizeof *out);
    if (!r) return fail(err, cap, "out of memory");
    yjs_arena_init_heap(&a, 0);
    /* yexp_migrated() gives a version 2 file's canonical bytes and a version
     * 1 file's migrated ones: the designer writes version 2 only. A text the
     * loader refuses is kept as typed, so no work is lost (designer.md). */
    if (yexp_load(&a, text, n, NULL, &d, r) == YEXP_OK && d && yexp_migrated(d, &a, &bytes, &len, NULL) == YEXP_OK) {
        out->canonical = true;
    } else {
        bytes = text;
        len = n;
    }
    copy = dup_bytes(bytes, len);
    yjs_arena_free(&a);
    free(r);
    if (!copy) return fail(err, cap, "out of memory");
    if (dsg_write_file(p->def_path, copy, len, err, cap)) {
        free(copy);
        out->canonical = false;
        return -1;
    }
    free(p->saved);
    p->saved = copy;
    p->saved_len = len;
    p->migrated = false;
    out->bytes = dup_bytes(copy, len);
    out->n = out->bytes ? len : 0;
    if (!out->bytes) return fail(err, cap, "saved; then out of memory");
    return 0;
}

/* --- create ---------------------------------------------------------------- */

/* One trial, so the project checks clean from the start. */
static const char k_new_definition[] =
    "{\"format\": \"ysp-experiment\", \"version\": 2,"
    " \"stimuli\": {\"fixation\": {\"kind\": \"shape\", \"shape\": \"cross\", \"w\": 20, \"h\": 20}},"
    " \"trials\": {\"fixate\": {\"events\": [{\"on\": \"fixation\"}, {\"wait_s\": 0.5}, {\"off\": \"fixation\"}]}},"
    " \"timeline\": [{\"name\": \"main\", \"trial\": \"fixate\"}]}";

static const char k_new_description[] =
    "{\n"
    "  \"format\": \"ysp-pack-source\",\n"
    "  \"version\": 1,\n"
    "  \"experiment\": { \"entry\": \"experiment/main.json\", \"source\": \"experiment.json\",\n"
    "                  \"media_type\": \"application/vnd.ysp.experiment\" },\n"
    "  \"resources\": []\n"
    "}\n";

int dsg_project_create(const char* folder, char* err, size_t cap) {
    char desc[DSG_PATH_MAX], def[DSG_PATH_MAX];
    yjs_arena a;
    yexp_def* d = NULL;
    yexp_report* r;
    const char* bytes;
    size_t n;
    int rc;
    if (join(desc, sizeof desc, folder, DSG_DESC_FILE) || join(def, sizeof def, folder, "experiment.json"))
        return fail(err, cap, "'%s': the path is too long", folder);
    if (dsg_make_dir(folder, err, cap)) return -1;
    if (dsg_is_file(desc)) return fail(err, cap, "'%s' has a %s already: open it", folder, DSG_DESC_FILE);
    if (dsg_is_file(def)) return fail(err, cap, "'%s' has an experiment.json already: it is not overwritten", folder);
    r = (yexp_report*)malloc(sizeof *r);
    if (!r) return fail(err, cap, "out of memory");
    yjs_arena_init_heap(&a, 0);
    rc = yexp_load(&a, k_new_definition, sizeof k_new_definition - 1, NULL, &d, r);
    if (rc == YEXP_OK) rc = yexp_canonical(d, &a, &bytes, &n, NULL);
    if (rc != YEXP_OK) rc = fail(err, cap, "the new definition: %s", r->n_diags ? r->diags[0].msg : "refused");
    else rc = dsg_write_file(def, bytes, n, err, cap);
    yjs_arena_free(&a);
    free(r);
    if (rc) return -1;
    return dsg_write_file(desc, k_new_description, sizeof k_new_description - 1, err, cap);
}
