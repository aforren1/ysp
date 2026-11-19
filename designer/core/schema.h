/* schema.h - what the panels show for each key of a format 2 definition
 * (step 4, part C; docs/designer.md, "Plan of step 4"). OWNED BY PART C: C
 * may change this file and schema.c as it needs.
 *
 * The keys, their types, defaults, enum values and whether they are
 * required come from the format's own table, yexp_defaults(2)
 * (ysp/experiment.h, format D2, D41). The unit, the range and the text of a
 * key that a header's desc also has come from that header's parameter
 * table (format G6): ygfx_params() and yrdk_params() for stimuli so far.
 * A duration (a key that ends in _s) is in seconds (D4). Nothing here is a
 * rule: the player's loader stays the only validator.
 */
#ifndef YSP_DESIGNER_SCHEMA_H
#define YSP_DESIGNER_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Value types: the order of ysp/experiment.h's internal YEXP__T_* (0.5.3).
 * schema.c checks the mirror where it can (schema_test); prerequisite P1
 * of the plan makes the header's own enum public, and this mirror goes. */
enum {
    DSG_T_NUM, DSG_T_INT, DSG_T_DUR, DSG_T_BOOL, DSG_T_STR, DSG_T_ENUM, DSG_T_ENUMS,
    DSG_T_TEXT, DSG_T_RES, DSG_T_VEC3, DSG_T_NUMS, DSG_T_GRID, DSG_T_VAL, DSG_T_CODE,
    DSG_T_VAR, DSG_T_NAME, DSG_T_NAMEZ, DSG_T_NAMES, DSG_T_STRS, DSG_T_TARGETS,
    DSG_T_SMAP, DSG_T_ANY, DSG_T_OBJ, DSG_T_SET, DSG_T_LIST,
    DSG_T_POINT, DSG_T_CENTER, DSG_T_TRANSFORM, DSG_T_REF, DSG_T_REFS, DSG_T_SINES,
    DSG_T_BOX, DSG_T_OUTCOME,
    DSG_T_COUNT
};

typedef struct dsg_field {
    const char* key;
    int         section;   /* yexp_section_name(section): "stimulus", "exit" */
    uint16_t    kinds;     /* the format's row: which kinds of the section have it */
    int         type;      /* DSG_T_*                                         */
    bool        required;
    const char* def;       /* the default's canonical JSON; NULL: none        */
    const char* values;    /* an enum's "name=value,..."; NULL otherwise      */
    const char* unit;      /* "s", a header's unit ("px", "deg", "u"), or ""  */
    const char* doc;       /* a header's text; "" when none has the key       */
    double      min, max;  /* a header's range; NaN when none                 */
    const char* from;      /* "format", or the header that gave unit and doc  */
} dsg_field;

/* Every field of format version 2, in the table's order (by section, then
 * key). Built at the first call; NULL when out of memory. */
const dsg_field* dsg_schema_fields(int* n);
/* A section's number by its name ("stimulus"), or -1. */
int  dsg_schema_section(const char* name);
/* The fields of one section and kind: kind_bit is 1 << the kind's number
 * (yexp_stim_kind, yexp_step_op, yexp_control_kind), or 0xffff for a
 * section without kinds. Writes at most cap pointers; returns the count. */
int  dsg_schema_select(int section, unsigned kind_bit, const dsg_field** out, int cap);

#ifdef __cplusplus
}
#endif
#endif
