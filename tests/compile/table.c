/* Compile check: ysp/table.h as a C translation unit with the implementation
 * and the opt-in file loader enabled. CMake and CI build this as C11 with
 * warnings as errors; table.cpp builds the same source as C++17. The
 * header has no threads. It parses a two-line table and returns 0 to prove
 * it links and runs. The table names the lifecycle by type, so a signature
 * change fails here and not in a user's build. */
#define YSP_TABLE_IMPLEMENTATION
#define YTB_STDIO
#include "ysp/table.h"

#include <string.h>

typedef struct table_api {
    bool (*open_csv)(ytb_table*, const ytb_csv_desc*);
    bool (*open_csv_file)(ytb_table*, const char*, const ytb_csv_desc*);
    bool (*open_view)(ytb_table*, const void*, size_t);
    void (*close)(ytb_table*);
    int (*parse_number)(const char*, size_t, double*);
} table_api;

static const table_api g_table_api = {
    ytb_open_csv, ytb_open_csv_file, ytb_open_view, ytb_close, ytb_parse_number
};

int main(void) {
    static uint64_t arena[1024];
    ytb_table t, v;
    ytb_csv_desc d;
    double x;
    int ok;
    static const char csv[] = "a,b\n1,x\n";
    memset(&d, 0, sizeof(d));
    d.text = csv;
    d.len = sizeof(csv) - 1;
    d.arena = arena;
    d.arena_size = sizeof(arena);
    if (!g_table_api.open_csv(&t, &d) || ytb_int(&t, 0, 0) != 1) return 1;
    if (!g_table_api.open_view(&v, t.base, t.size)) return 2;
    if (g_table_api.parse_number("0,5", 3, &x) >= 0) return 3;
    ok = g_table_api.open_csv_file(&v, NULL, &d) ? 4 : 0;
    g_table_api.close(&v);
    g_table_api.close(&v);
    g_table_api.close(&t);
    return ok;
}
