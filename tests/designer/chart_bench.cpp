// chart_bench.cpp - what the state chart costs a frame (designer/app/
// view_chart.cpp, designer/core/chart.c; docs/designer.md, "The chart").
// Not a test: it prints times. Linux and macOS (clock_gettime).
//
// Usage: bench_designer_chart WORK [FRAMES]
//   Charts: v2_retry (tests/experiment/good; 8 states and 21 edges, the
//   largest chart in the repository) and made-up templates of 50 and 200
//   states, each with an exit to the next state, an after_s exit to the end
//   on every third, one over and a shared exit in every state. Each at zoom
//   1 (most of a large chart off the canvas) and at zoom 0.25 (all of it on
//   the canvas).
//   For each, after 60 frames that fill the glyph atlas, FRAMES frames
//   (default 1000) with no input, headless as test_designer_ui draws them:
//     draw     the chart view's draw() alone, in a 1000 x 700 window
//     ring     the same with the preview's place on the middle state (a
//              test double, view_chart.h) and Follow on (step 5, part C)
//     frame    a whole frame (NewFrame to Render) with the chart shown
//     build    dsg_chart_build() of the template (each change of the text)
// Each line: median, p99 and max in microseconds.
#include "ui.h"
#include "views.h"
#include "view_chart.h"
#include "chart.h"
#include "skt.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_SOURCE_DIR
#define YSP_SOURCE_DIR "."
#endif
#ifndef YSP_DESIGNER_FONT_DIR
#define YSP_DESIGNER_FONT_DIR "fonts"
#endif

namespace {

dsg_preview_where g_where;
const dsg_preview_where* fake_where() { return &g_where; }

double now_us() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int cmp_d(const void* a, const void* b) {
    const double x = *(const double*)a, y = *(const double*)b;
    return x < y ? -1 : x > y;
}

void report(const char* what, const char* name, double* t, int n) {
    qsort(t, (size_t)n, sizeof *t, cmp_d);
    printf("  %-6s %-24s median %8.1f  p99 %8.1f  max %8.1f us\n", what, name, t[n / 2], t[(n * 99) / 100], t[n - 1]);
}

void accept_textures() {
    for (ImTextureData* t : ImGui::GetPlatformIO().Textures) {
        if (t->Status == ImTextureStatus_WantCreate || t->Status == ImTextureStatus_WantUpdates) {
            t->SetTexID((ImTextureID)1);
            t->SetStatus(ImTextureStatus_OK);
        } else if (t->Status == ImTextureStatus_WantDestroy) {
            t->SetTexID(ImTextureID_Invalid);
            t->SetStatus(ImTextureStatus_Destroyed);
        }
    }
}

double frame() {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    // outside the time: the check runs on its own thread
    ui_wait_checks(30000000000LL);
    const double t0 = now_us();
    ImGui::NewFrame();
    skt_frame_begin();
    ui_frame();
    skt_frame_end();
    ImGui::Render();
    const double t1 = now_us();
    accept_textures();
    return t1 - t0;
}

// The chart view's draw() alone, in a window of the size the main area has.
double draw_only(const UiView* v) {
    ImGui::GetIO().DeltaTime = 1.0f / 60.0f;
    ImGui::NewFrame();
    skt_frame_begin();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(ImVec2(1000, 700));
    ImGui::Begin("bench", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings);
    const double t0 = now_us();
    v->draw();
    const double t1 = now_us();
    ImGui::End();
    skt_frame_end();
    ImGui::Render();
    accept_textures();
    return t1 - t0;
}

bool write(const char* path, const char* s, size_t n) {
    char err[256];
    if (dsg_write_file(path, s, n, err, sizeof err) == 0) return true;
    printf("%s\n", err);
    return false;
}

// A project: bimanual's pack.json, the definition, and a view file with
// the chart's template and zoom, and Follow on.
bool project(const char* dir, const char* def, size_t n, const char* tmpl, float zoom) {
    char err[256], a[DSG_PATH_MAX], b[DSG_PATH_MAX];
    char* d = nullptr;
    size_t dn = 0;
    if (dsg_make_dir(dir, err, sizeof err)) return false;
    snprintf(a, sizeof a, "%s/tests/player/bimanual/pack.json", YSP_SOURCE_DIR);
    if (dsg_read_file(a, &d, &dn, err, sizeof err)) return false;
    bool ok = dsg_join(b, sizeof b, dir, "pack.json") == 0 && write(b, d, dn);
    free(d);
    ok = ok && dsg_join(b, sizeof b, dir, "experiment.json") == 0 && write(b, def, n);
    dsg_view v;
    memset(&v, 0, sizeof v);
    char part[512];
    snprintf(part, sizeof part, "{\"follow\": true, \"template\": \"%s\", \"templates\": {\"%s\": {\"pan\": [20, 20], \"zoom\": %.2f, \"states\": {}}}}",
             tmpl, tmpl, zoom);
    snprintf(v.main_view, sizeof v.main_view, "chart");
    ok = ok && dsg_view_part_set(&v, "chart", part, strlen(part)) == 0 && dsg_view_write(dir, &v, err, sizeof err) == 0;
    dsg_view_free(&v);
    return ok;
}

// n states, each with an exit to the next; every third with an after_s
// exit to the end; one over; one shared exit in every state.
char* made_up(int n, size_t* len) {
    ImVector<char> s;
    char b[256];
    auto put = [&](const char* t) { for (const char* p = t; *p; p++) s.push_back(*p); };
    put("{\"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"big\",\n"
        " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_px\"}},\n"
        " \"trials\": {\"big\": {\"controls\": {\"c\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 5, \"stay_s\": 0.1},"
        " \"lost\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 500, \"outside\": true}},\n"
        "  \"exits\": [{\"on\": \"lost\", \"to\": \"end\", \"in\": [");
    for (int i = 0; i < n; i++) {
        snprintf(b, sizeof b, "%s\"s%d\"", i ? ", " : "", i);
        put(b);
    }
    put("]}],\n  \"states\": [\n");
    for (int i = 0; i < n; i++) {
        char to[32];
        if (i + 1 < n) snprintf(to, sizeof to, "s%d", i + 1);
        else snprintf(to, sizeof to, "end");
        snprintf(b, sizeof b, "   {\"name\": \"s%d\", %s\"exits\": [{\"on\": \"c\", \"to\": \"%s\"}%s]}%s\n", i,
                 i == 0 ? "\"max_visits\": 3, \"over\": {\"to\": \"end\"}, " : "", to,
                 i % 3 == 2 ? ", {\"after_s\": 1, \"to\": \"end\"}" : "", i + 1 < n ? "," : "");
        put(b);
    }
    put("  ]}},\n \"timeline\": [{\"trial\": \"big\"}]}\n");
    char* out = (char*)malloc((size_t)s.Size + 1);
    memcpy(out, s.Data, (size_t)s.Size);
    out[s.Size] = 0;
    *len = (size_t)s.Size;
    return out;
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: bench_designer_chart WORK [FRAMES]\n");
        return 2;
    }
    const char* work = argv[1];
    const int frames = argc > 2 ? atoi(argv[2]) : 1000;
    char err[512];
    if (dsg_make_dir(work, err, sizeof err)) return 1;
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    if (!ui_init(YSP_DESIGNER_FONT_DIR, nullptr, err, sizeof err)) {
        printf("ui_init: %s\n", err);
        return 1;
    }
    int nv = 0;
    const UiView* const* views = ui_views(&nv);
    const UiView* chart = nullptr;
    for (int i = 0; i < nv; i++)
        if (strcmp(views[i]->id, "chart") == 0) chart = views[i];
    if (!chart) return 1;

    struct Case {
        const char* name;
        int states;   // 0: v2_retry
        float zoom;
    };
    static const Case cases[] = {
        { "v2_retry, zoom 1", 0, 1.0f },   { "50 states, zoom 1", 50, 1.0f },   { "50 states, zoom 0.25", 50, 0.25f },
        { "200 states, zoom 1", 200, 1.0f }, { "200 states, zoom 0.25", 200, 0.25f },
    };
    double* t = (double*)malloc(sizeof(double) * (size_t)frames);
    for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        char dir[DSG_PATH_MAX], sub[64];
        char* def = nullptr;
        size_t n = 0;
        snprintf(sub, sizeof sub, "case%d", (int)c);
        if (dsg_join(dir, sizeof dir, work, sub)) return 1;
        if (cases[c].states == 0) {
            char path[DSG_PATH_MAX];
            snprintf(path, sizeof path, "%s/tests/experiment/good/v2_retry.json", YSP_SOURCE_DIR);
            if (dsg_read_file(path, &def, &n, err, sizeof err)) return 1;
        } else {
            def = made_up(cases[c].states, &n);
        }
        const char* tmpl = cases[c].states ? "big" : "trial";
        if (!project(dir, def, n, tmpl, cases[c].zoom)) return 1;
        free(def);
        ui_open(dir);
        for (int i = 0; i < 60; i++) frame();
        if (strcmp(ui_main_view(), "chart") != 0) {
            printf("the chart is not shown\n");
            return 1;
        }
        dsg_chart model = {};
        const yjs_value* root = dsg_doc_root(ui_doc());
        if (dsg_chart_build(root, tmpl, &model)) return 1;
        printf("%s: %d states, %d edges, %d errors in the check\n", cases[c].name, model.n_states, model.n_edges,
               ui_report() ? ui_report()->n_errors : -1);
        for (int i = 0; i < frames; i++) t[i] = draw_only(chart);
        report("draw", cases[c].name, t, frames);
        memset(&g_where, 0, sizeof g_where);
        g_where.trial = 0;
        g_where.condition = -1;
        g_where.tmpl = tmpl;
        g_where.state = model.states[model.n_states / 2].name;
        const ChartPreviewHooks hooks = { fake_where, nullptr };
        ui_chart_set_preview_hooks(&hooks);
        for (int i = 0; i < frames; i++) t[i] = draw_only(chart);
        report("ring", cases[c].name, t, frames);
        ui_chart_set_preview_hooks(nullptr);
        for (int i = 0; i < frames; i++) t[i] = frame();
        report("frame", cases[c].name, t, frames);
        for (int i = 0; i < frames; i++) {
            const double t0 = now_us();
            dsg_chart_build(root, tmpl, &model);
            t[i] = now_us() - t0;
        }
        report("build", cases[c].name, t, frames);
        dsg_chart_free(&model);
    }
    free(t);
    ui_shutdown();
    ImGui::DestroyContext();
    return 0;
}
