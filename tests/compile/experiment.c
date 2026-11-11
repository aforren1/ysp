/* Compile check: ysp/experiment.h as a C translation unit with the
 * implementation. CMake and CI build this as C11 with warnings as errors;
 * experiment.cpp builds the same source as C++17. It loads a small
 * definition from a fixed arena, gets the canonical bytes and the
 * experiment ID, and returns 0 to prove it links and runs. */
#define YSP_JSON_IMPLEMENTATION
#include "ysp/json.h"
#define YSP_EXPERIMENT_IMPLEMENTATION
#include "ysp/experiment.h"

#include <string.h>

int main(void) {
    static uint64_t mem[1 << 16];
    static yexp_report report;
    static const char text[] =
        "{\"format\": \"ysp-experiment\", \"version\": 1,"
        " \"trials\": {\"t\": {\"events\": [{\"wait_s\": 0.5}]}},"
        " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\"}]}";
    yjs_arena a;
    yexp_def* d;
    const char* bytes;
    size_t n;
    uint8_t id[32];
    yjs_arena_init(&a, mem, sizeof mem);
    if (yexp_load(&a, text, sizeof text - 1, NULL, &d, &report) != YEXP_OK) return 1;
    if (d->n_templates != 1 || d->n_timeline != 1 || d->templates[0].n_events != 1) return 2;
    if (d->templates[0].events[0].t->i != 500000000) return 3;
    if (yexp_canonical(d, &a, &bytes, &n, id) != YEXP_OK || n < 2 || bytes[n - 1] != '\n') return 4;
    {
        /* format version 2: a signal role, a control, two states */
        static const char text2[] =
            "{\"format\": \"ysp-experiment\", \"version\": 2,"
            " \"roles\": {\"hand\": {\"type\": \"position2d\", \"units\": \"display_px\"}},"
            " \"trials\": {\"t\": {\"controls\": {\"home\": {\"control\": \"region\", \"of\": \"hand\", \"radius\": 10}},"
            " \"states\": [{\"name\": \"wait\", \"exits\": [{\"on\": \"home\", \"to\": \"done\"}]}, {\"name\": \"done\"}]}},"
            " \"timeline\": [{\"name\": \"main\", \"trial\": \"t\"}]}";
        const yexp_control* home;
        yjs_arena_init(&a, mem, sizeof mem);
        if (yexp_load(&a, text2, sizeof text2 - 1, NULL, &d, &report) != YEXP_OK) return 6;
        home = yexp_control_get(d, &d->templates[0], "home");
        if (d->format_version != 2 || !yexp_sigrole_get(d, "hand") || !home || home->kind != YEXP_REGION || home->of.kind != YEXP_REF_ROLE) return 7;
        if (d->templates[0].n_states != 2 || d->templates[0].states[0].exits[0].to.index != 1) return 8;
        if (yexp_migrated(d, &a, &bytes, &n, id) != YEXP_OK) return 9;
    }
    return strlen(yexp_version()) ? 0 : 5;
}
