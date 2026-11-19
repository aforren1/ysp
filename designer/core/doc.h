/* doc.h - the document that every view of the designer edits
 * (docs/designer.md, "Plan of step 4").
 *
 * The bytes are the source of truth. A document holds the project's files
 * as buffers of bytes: buffer 0 is the definition, the others are the
 * tables (CSV). Every change, from the text view, a chart, a table or a
 * panel, is a splice of a buffer's bytes, and one undo stack holds them all.
 * The parsed definition (a ysp/json.h tree) is a cache of buffer 0, made
 * again when its bytes change; a table's grid is its view's cache.
 *
 * Structured edits of the definition (dsg_def_*) work on a copy of the
 * parsed tree inside a transaction. The commit writes the tree in the
 * canonical layout (ysp/json.h YJS_WRITE_PRETTY, one LF after the value)
 * and splices the difference into buffer 0. They are refused while the
 * text does not parse: the text view is then the only editor, and the
 * other views show the last text that parsed, read-only.
 *
 * Paths name a value in the definition as a JSON Pointer (RFC 6901):
 * "" is the root, "/trials/reach/states/2/exits/0/to" a member of a member,
 * "~1" is '/' and "~0" is '~' in a key. dsg_def_set() and dsg_def_insert()
 * take "-" as "after the last item" of an array.
 *
 * Not thread safe: one thread owns a document. Every function that can
 * fail returns 0, or -1 with one line in dsg_doc_error().
 */
#ifndef YSP_DESIGNER_DOC_H
#define YSP_DESIGNER_DOC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ysp/json.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- buffers --------------------------------------------------------------- */

#define DSG_BUF_DEF     0     /* the definition: always buffer 0            */
#define DSG_MAX_BUFFERS 64
#define DSG_BUF_NAME    512   /* a buffer's name: its path in the project   */

enum { DSG_KIND_DEFINITION = 0, DSG_KIND_TABLE = 1 };

typedef struct dsg_doc dsg_doc;

dsg_doc* dsg_doc_new(void);
void     dsg_doc_free(dsg_doc* d);
/* Forgets every buffer, the undo stack and the selection, and makes buffer
 * 0 the definition with these bytes (saved: not dirty). name is its path in
 * the project ("experiment.json"). */
int      dsg_doc_reset(dsg_doc* d, const char* name, const char* bytes, size_t n);
/* Adds a buffer (a table) with these bytes, saved. Returns its index, or -1.
 * Not undoable: a project's files are loaded, not edited, into buffers. */
int      dsg_doc_add_buffer(dsg_doc* d, int kind, const char* name, const char* bytes, size_t n);
int      dsg_doc_buffer_count(const dsg_doc* d);
/* The index of the buffer with this name, or -1. */
int      dsg_doc_find_buffer(const dsg_doc* d, const char* name);
const char* dsg_doc_buffer_name(const dsg_doc* d, int buf);
int      dsg_doc_buffer_kind(const dsg_doc* d, int buf);
/* The bytes, with a NUL after the n bytes (not counted). Valid until the
 * buffer's next change. */
const char* dsg_doc_bytes(const dsg_doc* d, int buf, size_t* n);
/* A number that changes with every change of the buffer (an edit, an undo):
 * a view keeps the revision its cache was made from. */
uint64_t dsg_doc_rev(const dsg_doc* d, int buf);
/* Changes with every change of any buffer: the check runs when it moved. */
uint64_t dsg_doc_revision(const dsg_doc* d);
/* The buffer differs from its saved bytes. */
bool     dsg_doc_dirty(const dsg_doc* d, int buf);
/* The buffer's bytes are on disk now. */
void     dsg_doc_mark_saved(dsg_doc* d, int buf);
/* These bytes are on disk (a version 1 file shown migrated is dirty). */
int      dsg_doc_set_saved(dsg_doc* d, int buf, const char* bytes, size_t n);

/* --- changes and undo -------------------------------------------------------- */

/* Flags of dsg_doc_replace(). */
#define DSG_EDIT_MERGE 0x1u   /* typing: joins the last step when it is typing
                               * at the same place                          */

/* A transaction makes one undo step of every change between begin and
 * commit. Transactions nest; the outermost commit makes the step. A change
 * outside a transaction is a step of its own. label names the step for the
 * Undo and Redo buttons ("Rename state"). */
void dsg_doc_begin(dsg_doc* d, const char* label);
/* Writes a structured edit's tree to buffer 0, makes the step and calls the
 * rename listeners. 0, or -1 (and the transaction's changes are undone). */
int  dsg_doc_commit(dsg_doc* d);
/* Undoes every change since the outermost begin. */
void dsg_doc_cancel(dsg_doc* d);
bool dsg_doc_in_transaction(const dsg_doc* d);

/* Replaces bytes [off, off + del) of a buffer with ins[0, n). */
int  dsg_doc_splice(dsg_doc* d, int buf, size_t off, size_t del, const char* ins, size_t n);
/* Replaces a buffer's bytes with these; only the bytes that differ go in the
 * step (a common head and tail are kept). The text view calls this with the
 * editor's text and DSG_EDIT_MERGE. No change: no step. */
int  dsg_doc_replace(dsg_doc* d, int buf, const char* bytes, size_t n, unsigned flags);

bool dsg_doc_can_undo(const dsg_doc* d);
bool dsg_doc_can_redo(const dsg_doc* d);
/* The label of the step that undo or redo would take; "" when none. */
const char* dsg_doc_undo_label(const dsg_doc* d);
const char* dsg_doc_redo_label(const dsg_doc* d);
int  dsg_doc_undo(dsg_doc* d);
int  dsg_doc_redo(dsg_doc* d);
/* The place of the latest change, undo or redo: its buffer and the bytes
 * [*off, *off + *n) that it left (where the text view puts the caret).
 * false when nothing changed yet. */
bool dsg_doc_last_change(const dsg_doc* d, int* buf, size_t* off, size_t* n);
/* Steps on the stack, and their bytes (old and new text kept). */
int  dsg_doc_undo_depth(const dsg_doc* d);
size_t dsg_doc_undo_bytes(const dsg_doc* d);
/* Ends typing's merge: the next typed change is a step of its own. */
void dsg_doc_break_merge(dsg_doc* d);

const char* dsg_doc_error(const dsg_doc* d);

/* --- renames ----------------------------------------------------------------- */

/* View state keyed by a name (a state's position in the chart, a column's
 * width) follows a rename through these calls. scope names the set:
 * the definition's JSON Pointer of the object or array whose member or item
 * was renamed ("/trials/reach/states", "/stimuli"), or "#NAME/columns" for
 * the columns of the table in buffer NAME. Undo calls fn with from and to
 * swapped; redo as the commit did. */
typedef void (*dsg_rename_fn)(void* ctx, const char* scope, const char* from, const char* to);
int  dsg_doc_listen(dsg_doc* d, dsg_rename_fn fn, void* ctx);   /* at most 16 */
void dsg_doc_unlisten(dsg_doc* d, dsg_rename_fn fn, void* ctx);
/* Inside a transaction: the step renames from to in scope. dsg_def_rename()
 * notes its own rename. */
int  dsg_doc_note_rename(dsg_doc* d, const char* scope, const char* from, const char* to);

/* --- selection ---------------------------------------------------------------- */

#define DSG_SEL_PATH 512

/* What is selected, shared by every view. A step keeps the selection from
 * before its begin and after its commit; undo and redo put them back. */
typedef struct dsg_sel {
    int     buf;                 /* DSG_BUF_DEF, a table's buffer; -1 none  */
    char    path[DSG_SEL_PATH];  /* the definition: a JSON Pointer          */
    int32_t row, col;            /* a table: the cell, -1 for a whole column
                                  * or row; the definition: -1             */
} dsg_sel;

void dsg_sel_none(dsg_sel* s);
void dsg_sel_def(dsg_sel* s, const char* path);
void dsg_doc_select(dsg_doc* d, const dsg_sel* s);
const dsg_sel* dsg_doc_selection(const dsg_doc* d);
/* Changes with every dsg_doc_select() and every undo or redo that moves it. */
uint64_t dsg_doc_selection_rev(const dsg_doc* d);

/* --- the parsed definition ---------------------------------------------------- */

/* The tree of buffer 0's bytes, parsed again when they changed; NULL when
 * they do not parse (dsg_doc_parse_error() says where). Pointers into the
 * tree are valid until the next call of dsg_doc_root() or dsg_doc_shown()
 * after a change of buffer 0: views call it once a frame and keep no
 * pointer across frames. */
const yjs_value* dsg_doc_root(dsg_doc* d);
const yjs_error* dsg_doc_parse_error(dsg_doc* d);
/* The tree to show: the current one, or else the last tree that a call of
 * dsg_doc_root() or dsg_doc_shown() made (*stale true; NULL when none did).
 * Views call one of them each frame, so that is the text of the frame
 * before the text stopped parsing. A view shows a stale tree read-only. */
const yjs_value* dsg_doc_shown(dsg_doc* d, bool* stale);

/* --- structured edits of the definition --------------------------------------- */

/* Each works on the transaction's copy of the tree (opening a transaction
 * of its own when none is open), so later edits in a transaction see
 * earlier ones. -1 when the text does not parse, the path does not
 * resolve, or a value does not parse. */

/* The value at path in the transaction's tree, or in the current tree when
 * no transaction edits the definition; NULL when there is none. */
const yjs_value* dsg_def_get(dsg_doc* d, const char* path);
/* Sets path's value from JSON text: an object member is added or replaced
 * (missing objects on the way are made), an array item replaced, "-"
 * appends. */
int dsg_def_set(dsg_doc* d, const char* path, const char* json, size_t n);
int dsg_def_set_string(dsg_doc* d, const char* path, const char* utf8);
int dsg_def_set_int(dsg_doc* d, const char* path, int64_t v);
int dsg_def_set_bool(dsg_doc* d, const char* path, bool v);
/* A duration: the exact decimal of its nanoseconds (format D2, D4). */
int dsg_def_set_seconds_ns(dsg_doc* d, const char* path, int64_t ns);
/* A number as typed ("0.50"); the writer spells it canonically. */
int dsg_def_set_number_text(dsg_doc* d, const char* path, const char* text);
/* Removes an object member or an array item. */
int dsg_def_remove(dsg_doc* d, const char* path);
/* Inserts into the array at array_path before item index (index = n or -1:
 * at the end). */
int dsg_def_insert(dsg_doc* d, const char* array_path, int index, const char* json, size_t n);
/* Moves item from to position to in the array. */
int dsg_def_move(dsg_doc* d, const char* array_path, int from, int to);
/* Renames a member of the object at object_path, keeping its value, and
 * notes the rename. Refuses a key the object has. Does not change any
 * reference to the old name: the caller does that in the same transaction. */
int dsg_def_rename(dsg_doc* d, const char* object_path, const char* from, const char* to);

/* --- paths --------------------------------------------------------------------- */

/* base + "/" + key, with key escaped. -1 when it does not fit. */
int  dsg_path_key(char* out, size_t cap, const char* base, const char* key);
int  dsg_path_index(char* out, size_t cap, const char* base, int index);
/* The value at path under root; NULL when there is none. */
const yjs_value* dsg_path_get(const yjs_value* root, const char* path);
/* The parent's path into out, and the last token unescaped into last
 * (either may be NULL). -1 for the root or when it does not fit. */
int  dsg_path_split(const char* path, char* parent, size_t pcap, char* last, size_t lcap);
/* path is prefix or a value under it ("/a/b" is within "/a", "/ab" is not). */
bool dsg_path_within(const char* path, const char* prefix);
/* The innermost value whose key or start is at or before offset in the text
 * that root was parsed from: an issue's place to the value it names. "" for
 * the root. -1 when it does not fit. */
int  dsg_path_at(const yjs_value* root, uint32_t offset, char* out, size_t cap);
/* The byte offset of path's key (an object member) or start (an array item,
 * the root) in the text root was parsed from; false when there is none. */
bool dsg_path_offset(const yjs_value* root, const char* path, uint32_t* offset);

#ifdef __cplusplus
}
#endif
#endif
