/* Compile check: ysp/json.h as a C translation unit with the
 * implementation. CMake and CI build this as C11 with warnings as errors;
 * json.cpp builds the same source as C++17. The header has no threads. It
 * parses a small document into a fixed arena, writes it back in the
 * canonical form and returns 0 to prove it links and runs. */
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"

#include <string.h>

int main(void) {
    static uint64_t mem[512];
    static const char text[] = "{\"b\": [1, 2.5], \"a\": \"x\"}";
    char out[128];
    yjs_arena a;
    yjs_value* v;
    yjs_error e;
    int64_t one = 0;
    yjs_arena_init(&a, mem, sizeof mem);
    if (yjs_parse(&a, text, sizeof text - 1, NULL, &v, &e) != YJS_OK) return 1;
    if (yjs_int64(yjs_at(yjs_get(v, "b"), 0), &one) != YJS_OK || one != 1) return 2;
    if (yjs_write_mem(v, YJS_WRITE_COMPACT, out, sizeof out) != 21) return 3;
    if (strcmp(out, "{\"a\":\"x\",\"b\":[1,2.5]}") != 0) return 4;
    /* v0.2.0: a double's canonical text, integer arithmetic only */
    if (yjs_format_double(0.1 + 0.2, out, YJS_DOUBLE_TEXT) != 19 || strcmp(out, "0.30000000000000004") != 0) return 5;
    return 0;
}
