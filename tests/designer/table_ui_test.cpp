// table_ui_test.cpp - the tables' grid (designer/app/view_tables.cpp) in the
// designer's UI with no window and no GL, as test_designer_ui drives it
// (step 4, part B; docs/designer.md, "The tables"). OWNED BY PART B.
//
// Usage: test_designer_table_ui WORK [--bench PASSES]
//   1. A project with a table of 10,000 rows and 8 columns (the probe's
//      table: numbers and labels in mixed scripts) opens in the Tables view.
//   2. Only visible rows are laid out: at the top, the middle and the end
//      of the table, each frame lays out at most the visible rows' cells and
//      the header's, and draws the rows in view (the last row at the end).
//   3. A scroll through the whole table keeps that bound in every frame.
//   4. The shared layout cache (skt.h) frees the cells scrolled past.
//   5. A table edit marks the project unsaved: the title has its '*', and
//      Quit asks first.
//   6. A click on a table's issue in the issues panel shows the Tables view
//      with the issue's cell selected and in view.
//   --bench: PASSES scrolls through the table, 3 rows a frame, timing each
//   frame (NewFrame to Render); prints the median, p99 and max, cold (the
//   first pass) and warm, and skt_frame_begin() alone (the atlas's and the
//   layout cache's compaction), and the resident memory after each pass.
//   Not a test: run it pinned, on a quiet machine.
// Exit 0 on pass, 1 on a failure.
#include "ui.h"
#include "views.h"
#include "view_tables.h"
#include "skt.h"
#include "csv.h"

#include "imgui.h"
#include "imgui_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef YSP_DESIGNER_FONT_DIR
#define YSP_DESIGNER_FONT_DIR "fonts"
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static const int kRows = 10000;

static double now_s() {
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static double now_s();
static float begin_ms;   // the last frame's skt_frame_begin(): the caches' compaction

static void frame() {
    ImGuiIO& io = ImGui::GetIO();
    io.DeltaTime = 1.0f / 60.0f;
    // the check thread's result of the last frame's post lands in this one
    ui_wait_checks(30000000000LL);
    ImGui::NewFrame();
    const double b0 = now_s();
    skt_frame_begin();
    begin_ms = (float)((now_s() - b0) * 1e3);
    ui_frame();
    skt_frame_end();
    ImGui::Render();
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

// The probe's table (docs/designer_ui_probe.md, part 1): every label a
// different text, so every cell needs its own layout. The label is the
// second column, so it shows in the test's window.
static bool write_project(const char* dir) {
    static const char* const labels[] = {
        "Reach target %d",
        "\xD8\xA7\xD9\x84\xD9\x87\xD8\xAF\xD9\x81 \xD8\xB1\xD9\x82\xD9\x85 %d",
        "\xD7\x9E\xD7\x98\xD7\xA8\xD7\x94 \xD7\x9E\xD7\xA1\xD7\xA4\xD7\xA8 %d",
        "\xE0\xA4\xB2\xE0\xA4\x95\xE0\xA5\x8D\xE0\xA4\xB7\xE0\xA5\x8D\xE0\xA4\xAF %d",
        "go \xE2\x86\x92 \xD9\x85\xD8\xB1\xD8\xAD\xD8\xA8\xD8\xA7 %d",
        "\xD7\xA9\xD7\x9C\xD7\x95\xD7\x9D world \xE0\xA4\xA8\xE0\xA4\xAE\xE0\xA4\xB8\xE0\xA5\x8D\xE0\xA4\xA4\xE0\xA5\x87 %d",
    };
    static const char* const blocks[] = { "baseline", "rotation", "washout" };
    static const char* const fb[] = { "online", "endpoint", "none" };
    static const char desc[] =
        "{\n  \"format\": \"ysp-pack-source\",\n  \"version\": 1,\n"
        "  \"experiment\": { \"entry\": \"experiment/main.json\", \"source\": \"experiment.json\",\n"
        "                  \"media_type\": \"application/vnd.ysp.experiment\" },\n"
        "  \"resources\": [\n    { \"name\": \"big.pstb\", \"kind\": \"table\", \"source\": \"big.csv\" },\n"
        "    { \"name\": \"conditions.pstb\", \"kind\": \"table\", \"source\": \"conditions.csv\" }\n  ]\n}\n";
    static const char def[] =
        "{\n  \"format\": \"ysp-experiment\", \"version\": 2, \"name\": \"big_table\",\n"
        "  \"devices\": { \"resp\": { \"direction\": \"in\", \"family\": [\"keyboard\"] } },\n"
        "  \"stimuli\": { \"fix\": { \"kind\": \"shape\", \"shape\": \"cross\", \"w\": 16, \"h\": 16 } },\n"
        "  \"trials\": { \"t\": { \"events\": [{ \"on\": \"fix\" }, { \"wait_s\": 0.5 }],\n"
        "    \"response\": { \"role\": \"resp\", \"choices\": [\"left\", \"right\"], \"from\": \"fix\", \"duration_s\": 1, \"key_answer\": \"$key_answer\" } } },\n"
        "  \"timeline\": [{ \"name\": \"main\", \"timeline_variables\": \"conditions.pstb\", \"trial\": \"t\" }]\n}\n";
    static const char cond[] = "key_answer\nleft\nright\n";
    char path[DSG_PATH_MAX], err[512];
    ImVector<char> csv;
    char line[512];
    uint32_t r = 777;
    if (dsg_make_dir(dir, err, sizeof err)) return false;
    const char* head = "trial,label,block,target_x,target_y,rotation_deg,feedback,rt_s\n";
    for (const char* p = head; *p; p++) csv.push_back(*p);
    for (int i = 0; i < kRows; i++) {
        r = r * 1664525u + 1013904223u;
        const int a = (int)((r >> 8) % 8u) * 45;
        r = r * 1664525u + 1013904223u;
        const double rt = 0.25 + (double)(r >> 8) / 16777216.0 * 0.3;
        const int block = i < 2000 ? 0 : (i < 8000 ? 1 : 2);
        char label[128];
        snprintf(label, sizeof label, labels[i % 6], i + 1);
        const int n = snprintf(line, sizeof line, "%d,%s,%s,%d,%d,%d,%s,%.3f\n", i + 1, label, blocks[block], a, 360 - a,
                               block == 1 ? 30 : 0, fb[block == 2 ? 2 : 0], rt);
        for (int k = 0; k < n; k++) csv.push_back(line[k]);
    }
    return dsg_join(path, sizeof path, dir, "pack.json") == 0 && dsg_write_file(path, desc, sizeof desc - 1, err, sizeof err) == 0 &&
           dsg_join(path, sizeof path, dir, "experiment.json") == 0 && dsg_write_file(path, def, sizeof def - 1, err, sizeof err) == 0 &&
           dsg_join(path, sizeof path, dir, "conditions.csv") == 0 && dsg_write_file(path, cond, sizeof cond - 1, err, sizeof err) == 0 &&
           dsg_join(path, sizeof path, dir, "big.csv") == 0 && dsg_write_file(path, csv.Data, (size_t)csv.Size, err, sizeof err) == 0;
}

static const float kRowH = 28.0f;   // view_tables.cpp: ceil(16 px * 1.75)

// The most cells a frame may lay out: the rows the grid's window can show,
// one more for a row cut at each edge, and the header.
static int layout_bound() {
    ImGuiWindow* w = view_tables_grid_window();
    if (!w) return 0;
    const int rows = (int)(w->InnerRect.GetHeight() / kRowH) + 2;
    return (rows + 1) * 8;
}

static void scroll_to(float y) {
    ImGuiWindow* w = view_tables_grid_window();
    if (w) ImGui::SetScrollY(w, y);
    frame();
    frame();
}

// Resident memory, MB: the bench reports what a scroll leaves behind (the
// layout caches). Linux only; -1 elsewhere.
static double rss_mb() {
#ifdef __linux__
    FILE* f = fopen("/proc/self/statm", "r");
    long pages = 0, res = 0;
    if (!f) return -1;
    const int got = fscanf(f, "%ld %ld", &pages, &res);
    fclose(f);
    return got == 2 ? (double)res * 4096.0 / (1024.0 * 1024.0) : -1;
#else
    return -1;
#endif
}

static char title[DSG_PATH_MAX + 64];
static void set_title(void*, const char* t) { snprintf(title, sizeof title, "%s", t); }

static int cmp_float(const void* a, const void* b) {
    const float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y ? 1 : 0;
}

static void stats(const char* name, ImVector<float>& v) {
    qsort(v.Data, (size_t)v.Size, sizeof(float), cmp_float);
    const int n = v.Size;
    printf("BENCH %s frames=%d median=%.3f p99=%.3f max=%.3f ms\n", name, n, v[n / 2], v[(int)((n - 1) * 0.99 + 0.5)], v[n - 1]);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: test_designer_table_ui WORK [--bench PASSES]\n");
        return 2;
    }
    const int passes = argc > 3 && strcmp(argv[2], "--bench") == 0 ? atoi(argv[3]) : 0;
    char err[512], dir[DSG_PATH_MAX];
    if (dsg_make_dir(argv[1], err, sizeof err) || dsg_join(dir, sizeof dir, argv[1], "big") || !write_project(dir)) {
        printf("FAIL: the work folder: %s\n", err);
        return 1;
    }
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.DisplaySize = ImVec2(1400, 900);
    io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures | ImGuiBackendFlags_RendererHasVtxOffset;
    UiPlatform plat = {};
    plat.set_title = set_title;
    if (!ui_init(YSP_DESIGNER_FONT_DIR, &plat, err, sizeof err)) {
        printf("FAIL: ui_init: %s\n", err);
        return 1;
    }
    frame();
    ui_open(dir);
    CHECK(ui_project() != nullptr, "the project opens: %s", ui_status());
    CHECK(ui_show_view("tables"), "the tables view");
    for (int i = 0; i < 4; i++) frame();
    const dsg_report* rep = ui_report();
    CHECK(rep && rep->n_errors == 0, "the project checks clean: %d errors", rep ? rep->n_errors : -1);
    for (int i = 0; rep && rep->n_errors && i < rep->n_issues; i++) printf("  issue: %s\n", rep->issues[i].message);
    CHECK(view_tables_grid_window() != nullptr, "the grid drew");
    if (passes > 0) printf("BENCH opened: resident %.1f MB, %d layouts in the shared cache\n", rss_mb(), skt_layout_count());

    // 2. the top, the middle, the end
    int first = -1, last = -1;
    const int bound = layout_bound();
    const float max_y = view_tables_grid_window() ? view_tables_grid_window()->ScrollMax.y : 0;
    CHECK(max_y > (kRows - 40) * kRowH, "the grid scrolls over every row: %.0f px", max_y);
    const float at[3] = { 0.0f, max_y * 0.5f, max_y };
    for (int k = 0; k < 3; k++) {
        scroll_to(at[k]);
        const int n = view_tables_layouts_last_frame();
        view_tables_rows_last_frame(&first, &last);
        printf("  scroll %.0f px: rows %d to %d drawn, %d layouts (bound %d)\n", at[k], first, last, n, bound);
        CHECK(n > 8 && n <= bound, "scroll %.0f: %d layouts in a frame, at most %d", at[k], n, bound);
        CHECK(last - first + 1 <= bound / 8, "scroll %.0f: %d rows drawn", at[k], last - first + 1);
        CHECK(first <= (int)(at[k] / kRowH) + 1 && last >= (int)(at[k] / kRowH), "scroll %.0f: the rows in view (%d to %d)", at[k], first, last);
    }
    CHECK(last == kRows - 1, "at the end, the last row is drawn: %d", last);

    // 3. every frame of a scroll through the table
    int worst = 0, frames = 0;
    for (float y = 0; y <= max_y; y += 30 * kRowH) {
        ImGui::SetScrollY(view_tables_grid_window(), y);
        frame();
        worst = ImMax(worst, view_tables_layouts_last_frame());
        frames++;
    }
    CHECK(worst <= bound, "a scroll of %d frames: at most %d layouts a frame, bound %d", frames, worst, bound);

    // 4. The shared layout cache lets go of the rows scrolled past: 100
    // frames after their last use (skt.cpp), only the rows in view stay.
    const int after_scroll = skt_layout_count();
    const uint64_t made_scroll = skt_layouts_made();
    for (int i = 0; i < 101; i++) frame();
    // the evictions of these frames leave every row in view found
    CHECK(skt_layouts_made() == made_scroll, "the rows in view stay hits while the rest goes: %llu made",
          (unsigned long long)(skt_layouts_made() - made_scroll));
    printf("  shared layout cache: %d layouts after the scroll, %d after 101 frames at rest\n", after_scroll,
           skt_layout_count());
    CHECK(after_scroll <= 101 * bound + 64, "the scroll keeps at most 101 frames of cells: %d layouts", after_scroll);
    CHECK(skt_layout_count() <= bound + 64, "at rest, the cache keeps the cells in view: %d layouts, bound %d",
          skt_layout_count(), bound + 64);
    // A layout used every frame stays (a hit, not a new layout); the rows
    // in view are found again after the evictions (the count holds).
    const skb_layout_t* probe = skt_layout("cache probe", 16.0f);
    bool kept = probe != nullptr;
    const uint64_t made0 = skt_layouts_made();
    for (int i = 0; i < 150; i++) {
        frame();
        kept = kept && skt_layout("cache probe", 16.0f) == probe;
    }
    const int with_probe = skt_layout_count();
    CHECK(kept && skt_layouts_made() == made0, "150 frames at rest make no layout (%llu made): each one is a hit",
          (unsigned long long)(skt_layouts_made() - made0));
    for (int i = 0; i < 101; i++) frame();
    CHECK(skt_layout_count() == with_probe - 1, "101 frames unused, it goes: %d layouts, %d before", skt_layout_count(), with_probe);
    for (int i = 0; i < 20; i++) frame();
    CHECK(skt_layout_count() == with_probe - 1, "the rows in view are hits: %d layouts after 20 more frames", skt_layout_count());

    if (passes > 0) {
        ImVector<float> cold, warm, begin;
        printf("BENCH before the passes: resident %.1f MB, %d layouts in the shared cache\n", rss_mb(), skt_layout_count());
        for (int p = 0; p < passes; p++) {
            ImGui::SetScrollY(view_tables_grid_window(), 0);
            frame();
            for (float y = 0; y <= max_y; y += 3 * kRowH) {
                ImGui::SetScrollY(view_tables_grid_window(), y);
                const double t0 = now_s();
                frame();
                const float ms = (float)((now_s() - t0) * 1e3);
                (p == 0 ? cold : warm).push_back(ms);
                begin.push_back(begin_ms);
            }
            printf("BENCH after pass %d: resident %.1f MB, %d layouts in the shared cache\n", p + 1, rss_mb(), skt_layout_count());
        }
        printf("BENCH 10,000 rows x 8 columns, 3 rows a frame, %d layouts in the last frame\n", view_tables_layouts_last_frame());
        stats("cold", cold);
        if (warm.Size) stats("warm", warm);
        stats("skt_frame_begin", begin);
    }

    // 5. A cell edit through the document, as the grid's commit makes it,
    // while the text view shows: the definition does not change.
    CHECK(ui_show_view("text"), "the text view");
    frame();
    dsg_doc* d = ui_doc();
    const int buf = dsg_doc_find_buffer(d, "big.csv");
    dsg_csv idx = {};
    idx.buf = -1;
    CHECK(buf > 0 && dsg_csv_sync(&idx, d, buf, ',') == 0, "the table's buffer: %d", buf);
    CHECK(!ui_dirty() && title[0] != '*', "before the edit, saved: '%s'", title);
    const int row = 5000, col = 2;
    CHECK(dsg_csv_set_cell(d, &idx, row, col, "l\xFF", 2) == 0, "the edit: %s", idx.error);
    frame();
    frame();
    CHECK(ui_dirty(), "a table edit leaves the project unsaved");
    CHECK(title[0] == '*', "the title shows it: '%s'", title);

    // 6. The pack tool refuses the byte that is not UTF-8 and names the cell.
    const dsg_report* r6 = ui_report();
    int issue = -1;
    char entry[64];
    int prow = -2, pcol = -1;
    for (int i = 0; r6 && i < r6->n_issues; i++)
        if (dsg_csv_place(r6->issues[i].message, entry, sizeof entry, &prow, &pcol, nullptr, 0) && strcmp(entry, "big.pstb") == 0) issue = i;
    CHECK(issue >= 0, "the check names a cell of big.pstb");
    for (int i = 0; issue < 0 && r6 && i < r6->n_issues; i++) printf("  issue: %s\n", r6->issues[i].message);
    ui_jump_to_issue(issue);
    const dsg_sel* sel = dsg_doc_selection(d);
    CHECK(strcmp(ui_main_view(), "tables") == 0, "the issue shows the Tables view: '%s'", ui_main_view());
    CHECK(sel->buf == buf && sel->row == row && sel->col == col, "the issue's cell is selected: buffer %d, row %d, column %d",
          sel->buf, sel->row, sel->col);
    frame();
    frame();
    view_tables_rows_last_frame(&first, &last);
    CHECK(first <= row && row <= last, "the cell is in view: rows %d to %d", first, last);

    // 5, again: Quit asks before it drops the edit.
    CHECK(!ui_request_quit(), "Quit asks first");
    dsg_csv_free(&idx);

    ui_shutdown();
    ImGui::DestroyContext();
    printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "PASS", checks, fails);
    return fails ? 1 : 0;
}
