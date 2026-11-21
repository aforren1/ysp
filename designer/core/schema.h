/* schema.h - what the panels show for each key of a format 2 definition
 * (step 4, part C; docs/designer.md, "The panels"). OWNED BY PART C.
 *
 * The keys, their types, defaults, enum values and whether they are
 * required come from the format's own table, yexp_defaults(2)
 * (ysp/experiment.h, format D2, D41). The unit, the range and the text of a
 * key that a header's desc also has come from that header's parameter
 * table (format G6): ygfx_params(), ygfx_enum_params() and yrdk_params()
 * for stimuli, yrsp_params() for responses and choices, ysig_op_params()
 * for controls (through the op that a key lowers to, format 3.7), and
 * yst_params(), yqst_params() and yaep_params() for methods. A key that no
 * header's table has takes the format row's own unit and doc (0.6.0). A
 * duration (a key that ends in _s) is in seconds (D4).
 *
 * Nothing here is a rule: the player's loader stays the only validator. A
 * value that a header's range refuses is still written; the check says so.
 */
#ifndef YSP_DESIGNER_SCHEMA_H
#define YSP_DESIGNER_SCHEMA_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "doc.h"
#include "ysp/experiment.h"

#ifdef __cplusplus
extern "C" {
#endif

/* What a name in a key can name (format 3.9). A key's mask gives the
 * candidates its combo offers and the renames that change it. */
#define DSG_REF_STIM      0x0001u
#define DSG_REF_CTL_HITS  0x0002u   /* a control with hits: region, signal_lost */
#define DSG_REF_CTL_VALUE 0x0004u   /* a control with a value or a test          */
#define DSG_REF_CTL_MOVE  0x0008u   /* a movement (a signal choice)              */
#define DSG_REF_STATE     0x0010u   /* a state of the template                   */
#define DSG_REF_FLAG      0x0020u
#define DSG_REF_SIGROLE   0x0040u   /* a signal role (roles)                     */
#define DSG_REF_EVROLE    0x0080u   /* an event role (devices)                   */
#define DSG_REF_TEMPLATE  0x0100u
#define DSG_REF_METHOD    0x0200u
#define DSG_REF_VAR       0x0400u   /* "$name"                                   */
#define DSG_REF_RESPONSE  0x0800u   /* the word "response"                       */
#define DSG_REF_END       0x1000u   /* the word "end"                            */
#define DSG_REF_LANES     0x8000u   /* "NAME.x": a name with a lane              */
#define DSG_REF_CONTROL   (DSG_REF_CTL_HITS | DSG_REF_CTL_VALUE | DSG_REF_CTL_MOVE)

typedef struct dsg_field {
    const char* key;
    int         section;   /* yexp_section_name(section): "stimulus", "exit" */
    uint16_t    kinds;     /* which kinds of the section have it: the format
                            * row's kinds, split where a header's table
                            * differs per kind                              */
    int         type;      /* yexp_row_type: YEXP_T_*                         */
    unsigned    flags;     /* YEXP_R_*                                        */
    bool        required;
    const char* def;       /* the default's canonical JSON; NULL: none        */
    const char* values;    /* an enum's "name=value,..."; NULL otherwise      */
    const char* unit;      /* "s", a header's unit ("px", "deg", "u"), else
                            * the format row's; "" for none                  */
    const char* doc;       /* the format row's text, else a header's          */
    double      min, max;  /* a header's range, in the key's unit; NaN: none  */
    const char* from;      /* "format", or the header that gave unit and range */
    unsigned    refs;      /* DSG_REF_*: what the key names; 0: not a name    */
} dsg_field;

/* Every field of format version 2, in the table's order (by section, then
 * key). Built at the first call; NULL when out of memory. */
const dsg_field* dsg_schema_fields(int* n);
/* A section's number by its name ("stimulus"), or -1. */
int  dsg_schema_section(const char* name);
/* The fields of one section and kind: kind_bit is 1 << the kind's number
 * (yexp_stim_kind, yexp_step_op, yexp_control_kind; an exit 1, a shared
 * exit 2; a template 1, or 2 with states and no key_answer), or 0xffff for
 * a section without kinds. Rows that are a step's property (YEXP_R_STEP) are
 * left out. Writes at most cap pointers; returns the count. */
int  dsg_schema_select(int section, unsigned kind_bit, const dsg_field** out, int cap);
/* The field of key in a section and kind, or NULL. */
const dsg_field* dsg_schema_field(int section, unsigned kind_bit, const char* key);

/* --- the object at a path ------------------------------------------------------ */

/* What a JSON Pointer in a definition is part of. */
enum {
    DSG_OBJ_NONE = 0, DSG_OBJ_FILE, DSG_OBJ_DISPLAY, DSG_OBJ_SESSION, DSG_OBJ_SFIELD, DSG_OBJ_WEB,
    DSG_OBJ_EVROLE,      /* devices/NAME: an event role                     */
    DSG_OBJ_SIGROLE,     /* roles/NAME: a signal role                       */
    DSG_OBJ_STIMULUS, DSG_OBJ_METHOD,
    DSG_OBJ_CONTROL,     /* controls/NAME, top level or a template's        */
    DSG_OBJ_TEMPLATE,
    DSG_OBJ_RESPONSE,    /* a key response, or one with signal choices      */
    DSG_OBJ_RTRIGGER, DSG_OBJ_CHOICE, DSG_OBJ_STATE, DSG_OBJ_OVER,
    DSG_OBJ_EXIT,        /* a state's own exit                              */
    DSG_OBJ_SHARED_EXIT, /* a template's exits                              */
    DSG_OBJ_STEP,
    DSG_OBJ_COLUMN,      /* a template's columns/NAME                       */
    DSG_OBJ_NODE, DSG_OBJ_LOOP,
    DSG_OBJ_VAR,         /* a node's vars/NAME: a value, a choice, uniform, walk2d */
    DSG_OBJ_DATA,        /* a node's data/NAME: a constant column            */
    DSG_OBJ_COUNT
};

#define DSG_NAME_MAX 128

typedef struct dsg_obj {
    int      obj;                     /* DSG_OBJ_*                                 */
    int      section;                 /* its keys' section; -1: a value with no keys
                                       * (a column, a plain variable)               */
    unsigned kind_bit;                /* see dsg_schema_select(); 0: its kind is not
                                       * known (only the kind's own key shows)      */
    char     kind[32];                /* "grating", "region", "stair", "wait_s",
                                       * "uniform"; "" for none                     */
    char     path[DSG_SEL_PATH];      /* the object                                */
    char     keys[DSG_SEL_PATH];      /* where its keys are: path, or path/walk2d  */
    char     set[DSG_SEL_PATH];       /* a set member: the set ("/stimuli"); ""    */
    char     name[DSG_NAME_MAX];      /* the member's key, or the item's "name"    */
    char     tmpl[DSG_NAME_MAX];      /* the template it is in; ""                 */
    char     focus[DSG_NAME_MAX];     /* the key under the object that the path
                                       * names ("contrast"); ""                     */
} dsg_obj;

/* The innermost object that path is in (the root is the file), and its
 * section and kind from the values in the tree. 0, or -1 when path names
 * nothing in root. */
int  dsg_schema_object(const yjs_value* root, const char* path, dsg_obj* out);
/* The object at path and the ones that hold it, innermost first, as far as
 * the template (an exit, its state, its template). Returns the count. */
int  dsg_schema_chain(const yjs_value* root, const char* path, dsg_obj* out, int cap);
/* A name for the object kind: "stimulus", "shared exit". */
const char* dsg_schema_obj_name(int obj);
/* Whether a key of an object kind holds objects with panels of their own:
 * 1 one object (a template's response, a node's loop), 2 a set or a list of
 * them (stimuli, states, a state's exits), 0 none. */
int  dsg_schema_holds(int obj, const char* key);
/* Whether the object has a name that a rename changes (a set member, a
 * state, a node). */
bool dsg_schema_renamable(const dsg_obj* o);

/* --- values ----------------------------------------------------------------------- */

/* The key's path under the object: o->keys + "/" + key. -1 when it does not fit. */
int  dsg_schema_key_path(const dsg_obj* o, const char* key, char* out, size_t cap);
/* The text a field shows for a value: a string's own text for a string
 * value of a string type, else the value's canonical JSON. The default's
 * text when v is NULL. Returns the length (truncated to cap - 1). */
size_t dsg_schema_value_text(const dsg_field* f, const yjs_value* v, char* out, size_t cap);
/* True when the value fits the field's widget: a number for NUM, INT and DUR,
 * a bool for BOOL, a string for the string types. A variable, a lookup or
 * another form shows as JSON. NULL fits. */
bool dsg_schema_fits(const dsg_field* f, const yjs_value* v);
/* True for the types whose value is a list, a map or a form: the panel
 * shows them as JSON and opens the text view at the key. */
bool dsg_schema_complex(int type);

/* Writes typed text as the field's value at o's key, in one undo step:
 *   DUR: the decimal through yjs_parse_fixed(), as dsg_def_set_seconds_ns();
 *   NUM: the number as typed (dsg_def_set_number_text(): canonical bytes);
 *   INT: an integer; BOOL: true or false; the string types: the text;
 *   VAL, CODE, OUTCOME: a number, true, false, JSON, or else the text;
 *   the complex types: JSON.
 * For the types that are not strings, text that starts with '$' is a
 * variable, and text that starts with '{' or '[' is JSON (a lookup, a
 * form). With DSG_WRITE_JSON (the field showed the value as JSON: a lookup,
 * a form, a variable), text that starts with '{', '[' or '"' is JSON for
 * every type, and the rest is read as above. A value out of a header's
 * range is written: the check reports it. 0, or -1 with the reason in err
 * (the text does not read as the type, or the document refused the edit). */
#define DSG_WRITE_TYPED 0u
#define DSG_WRITE_JSON  1u
int  dsg_schema_write(dsg_doc* d, const dsg_obj* o, const dsg_field* f, const char* text, unsigned how, char* err, size_t cap);
/* Removes the key, so its default shows. No key: no step, 0. */
int  dsg_schema_reset(dsg_doc* d, const dsg_obj* o, const dsg_field* f, char* err, size_t cap);

/* --- names ------------------------------------------------------------------------- */

/* The names a key can take in the object's context (DSG_REF_* of f->refs),
 * NUL-separated in buf, in the tree's order: stimuli, controls (the
 * template's, then the top-level ones), states, flags, roles; "end" and
 * "response" where the key takes them; NAME.x and NAME.y for a key with
 * lanes. Returns the count; buf holds those that fit. */
int  dsg_schema_candidates(const yjs_value* root, const dsg_obj* o, const dsg_field* f, char* buf, size_t cap);

/* Renames the object (a set member, a state, a node) to `to`, and every
 * reference to it that the schema's name types find, in one undo step
 * "Rename": the keys whose DSG_REF_* mask has the object's kind, a lane
 * suffix kept ("hand.x"), "$NAME" for a variable, and the value forms
 * {"signal": N}, {"when": N}, {"hit": "N.x"}, {"time": N, "from": M}. A
 * state or a template's control is renamed within its template. A
 * variable's "$NAME" is changed only when no other node defines NAME; the
 * check reports any reference this misses. *changed gets the references
 * changed (may be NULL). 0, or -1 with the reason in err. */
int  dsg_schema_rename(dsg_doc* d, const dsg_obj* o, const char* to, int* changed, char* err, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
