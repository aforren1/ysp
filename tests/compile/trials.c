/* Compile check: ysp/trials.h as a C translation unit with the implementation
 * enabled. CMake and CI build this as C11 with warnings as errors;
 * trials.cpp builds the same source as C++17. The header has no threads,
 * so there is no YTR_NO_THREADS variant. It runs and returns 0 to prove it
 * links. The data writer's column macros are used in a static array,
 * which C needs to be constant expressions and C++ must accept as
 * aggregates. */
#define YSP_TRIALS_IMPLEMENTATION
#include "ysp/trials.h"

#include <string.h>

typedef struct check_row {
    int     key;
    double  level;
    int64_t rt_ns;
    char    code[4];
} check_row;

static const ytr_col check_cols[] = {
    YTR_COL(check_row, key, YTR_COL_INT),
    YTR_COL_D(check_row, level, YTR_COL_DOUBLE, 3),
    YTR_COL(check_row, rt_ns, YTR_COL_TIME_NS),
    YTR_COL(check_row, code, YTR_COL_CHARS),
    YTR_COL_SET("note", YTR_COL_STRING),
    YTR_COL_TEXT("participant", "P01"),
};

static bool discard(void* ctx, const void* bytes, size_t n) {
    (void)ctx;
    (void)bytes;
    (void)n;
    return true;
}

int main(void) {
    static ytr_data w;
    ytr_data_desc dd;
    check_row r = { 1, 0.5, 250000000, "ab" };
    memset(&dd, 0, sizeof(dd));
    dd.write = discard;
    dd.cols = check_cols;
    dd.n_cols = (int)(sizeof(check_cols) / sizeof(check_cols[0]));
    if (!ytr_data_open(&w, &dd)) return 1;
    if (ytr_data_set_string(&w, ytr_data_col(&w, "note"), "ok") != 0) return 1;
    if (ytr_data_write(&w, &r) != 0) return 1;
    return ytr_data_close(&w) ? 0 : 1;
}
