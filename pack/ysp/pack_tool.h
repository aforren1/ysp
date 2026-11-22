/* ysp/pack_tool.h - v0.12.0 - the pack tool as a library (prefix ypt_)
 *
 *   Builds, verifies, lists, extracts, rebuilds and appends ysp packs
 *   (docs/pack.md). The CLI `ypak` (pack/ypak.c) is a thin layer over these
 *   calls, and the designer runs the same code as WebAssembly. Not a single
 *   header: a C11 library in the pack tool's tree that links ysp/pack.h,
 *   ysp/json.h (every description and manifest), ysp/experiment.h and
 *   ysp/trials.h (the definition), ysp/table.h,
 *   ysp/outline.h, ysp/gfx.h (CPU parts: the shader wrapper and the curve
 *   set check), ysp/color.h, ysp/audio.h (the WAV probe), ysp/video.h
 *   (video indexes: pl_mpeg for MPEG-1, Media Foundation for MP4 on
 *   Windows), lodepng (PNG, and inflate for .xlsx), stb_image (JPEG)
 *   and, for glyph runs only, pack/layout (Skribidi and HarfBuzz). Built
 *   with YPT_NO_LAYOUT (CMake: YSP_BUILD_PACK=ON without YSP_BUILD_LAYOUT),
 *   the library has no pack/layout and refuses a GLYPHRUNS resource by
 *   name; every other kind gives the same bytes. The library compiles
 *   those headers' implementations itself (pack/pack_impl.c): a program
 *   that links it does not define YSP_PACK_IMPLEMENTATION,
 *   YSP_JSON_IMPLEMENTATION, YSP_TABLE_IMPLEMENTATION,
 *   YSP_OUTLINE_IMPLEMENTATION, YSP_GFX_IMPLEMENTATION,
 *   YSP_AUDIO_IMPLEMENTATION or YSP_VIDEO_IMPLEMENTATION again.
 *
 *   v0.1.1 (2026-10-09): the JSON reader and writer moved to ysp/json.h,
 *   the repository's one strict reader. Every pack of the test corpus is
 *   byte-identical to v0.1.0's but for the manifest, which now names
 *   ysp/json.h among the libraries.
 *
 *   v0.2.0 (2026-10-09): a TEXTURE entry's encoding bytes are what
 *   ygfx_texture() takes (ysp/gfx.h v0.10.5), so the documented load path
 *   works: "linear" stores all 0; "device" and "srgb" store primaries
 *   DEVICE, except sRGB codes in RGB, which need "primaries": "bt709" or
 *   "device" in the description (refused without); r16ui is linear only.
 *   Texture entries and the chunk list differ from v0.1.1's; so does a
 *   SHADER entry, which records YGFX_SHADER_CONTRACT 2.
 *
 *   v0.3.0 (2026-10-10): the core tool. pack/layout is optional
 *   (YPT_NO_LAYOUT), and the manifest's tool.libraries names "layout"
 *   only when the pack has a GLYPHRUNS entry. So a pack without glyph runs
 *   has the same bytes and pack ID from the core and the full tool; its
 *   manifest differs from v0.2.0's by that line, and every other entry is
 *   byte-identical to v0.2.0's.
 *
 *   v0.4.0 (2026-10-13): phase 3 (docs/pack.md 13). VIDEO and its
 *   VIDEO_INDEX from an MPEG-1 or MP4 file, as given or made by an ffmpeg
 *   step whose argument vector and version the manifest records (the tool
 *   encodes on one thread, `-threads 1` and libx265's pools=1, and refuses
 *   the author's thread settings, so any machine rebuilds the bytes; the
 *   user's decision of 2026-10-14); opt-in resampling; FRAMESEQ
 *   from a ysp/video.h frame sequence, every frame checked; a TABLE from an
 *   .xlsx sheet; a TEXTURE from a JPEG; an ICC profile in a PNG or JPEG is
 *   accepted when it is sRGB's and refused otherwise (v0.3.0 refused every
 *   iCCP). `ypak rebuild` takes an ffmpeg output from the pack when this
 *   ffmpeg is another version. The manifest names ysp/video.h, stb_image
 *   and pl_mpeg, so every pack ID differs from v0.3.0's; every other entry
 *   of the v0.3.0 test corpus is byte-identical.
 *
 *   v0.6.0 (2026-10-21): the experiment's glyph runs (gap G9 of
 *   docs/experiment_format.md; docs/pack.md 4.5 and 15). When the
 *   experiment entry is a version 1 definition, every text its text
 *   stimuli can show (literals, string-column levels of every table a node
 *   names, vars values and choices, every value of a lookup's map) is laid
 *   out into one GLYPHRUNS entry named after the experiment entry
 *   ("experiment/main.json" gives "experiment/main.ysprun"), keyed by
 *   text and style with ysp/pack.h's ypak_runs_key(), which the player
 *   calls too, each in its stimulus's font and style; a
 *   description lists no glyph runs for them. A glyph runs resource of that
 *   name adds its blocks and fallback fonts. A text known only at run time
 *   is refused by name (pack/derive.c, ypt_derive_texts()). A listed block
 *   without "size" is refused (v0.5.1 crashed). Every entry of the test
 *   corpus is byte-identical to v0.5.1's; its manifest differs by the
 *   tool version.
 *
 *   v0.8.0 (2026-11-09): the definition goes through ysp/experiment.h
 *   (docs/pack.md 16). A definition the loader refuses stops the build with
 *   its message; the entry is the loader's canonical bytes, keys at their
 *   defaults left out, so a pack whose definition writes one gets another
 *   experiment ID and pack ID than v0.7.0 gave. The tool gives the loader
 *   its entries, its tables' columns and levels and ytr_rules(); the rig,
 *   the parameter tables and ytl_check_ops() are skipped.
 *
 *   v0.10.0 (2026-11-18): glyph runs through pack/layout 0.2.0, whose
 *   font fallback goes past the script (Skribidi patch 0003): a digit or
 *   punctuation mark in Hebrew or Arabic text comes from the first listed
 *   font that has it, where v0.9.0 refused the block. A block still with
 *   glyph 0 is refused naming its characters, their runs' scripts and
 *   codepoints (at most 8, then a count), and only when no font in the list
 *   maps them; a glyph 0 for a character a font maps has its own message.
 *   Every pack with glyph runs gets a new manifest (the layout line lists
 *   three patch hashes and ylay 0.2.0) and pack ID; its entries are
 *   byte-identical.
 *
 *   v0.11.0 (2026-11-18): color glyphs (pack/layout 0.3.0, ysp/pack.h
 *   0.5.0, docs/pack.md 18). An emoji of a COLR font in a block's text is
 *   laid out as its layers in a paint run; the GLYPHRUNS entry is then
 *   version 2 (a run's flags above its 16-bit font index), the font's
 *   CURVESET holds the paint records first and the layer glyphs after the
 *   font's (flag YPAK_CSET_COLOR, "used" includes the layer glyphs the
 *   runs use), and the pack is format 2. Every other pack keeps format 1
 *   and its bytes. A color glyph the layers cannot draw exactly is refused
 *   by codepoint, glyph, font and feature, with what to use instead. A
 *   FONT whose color glyphs are CBDT, sbix or SVG with no COLR is refused;
 *   one with COLR beside them is stored without them (derivation op
 *   "drop_tables", the tables in "dropped"). The core tool refuses a
 *   CURVESET of a COLR font: flattening needs the layout library. Every
 *   pack with glyph runs gets a new manifest (four patch hashes, ylay
 *   0.3.0); entries without color glyphs are byte-identical.
 *
 *   v0.12.0 (2026-11-22): a build can be canceled, and derived media come
 *   from an entry cache (docs/pack.md 19; designer step 6, decisions 4 and
 *   17). ypt_options.cancel is read before each resource (so before its
 *   ffmpeg step) and after an ffmpeg step; a cancel gives YPT_ERR_CANCELED,
 *   no pack and no temporary file (an ffmpeg encode that runs goes to its
 *   end).
 *   ypt_options.cache names a folder of cache records: a TEXTURE, an AUDIO
 *   entry that is converted or resampled, and a VIDEO with its index are
 *   taken from a record when the record's key matches (the tool's and its
 *   libraries' versions, the resource, the project's audio, the source's
 *   SHA-256 and, for an ffmpeg step, the whole `ffmpeg -version` text and
 *   the command). A record is written through a temporary file and a
 *   rename; one that does not read back whole is a miss. ypt_options.
 *   cache_max and ypt_cache_trim() bound the folder. Without a cache, and
 *   with a cold or a warm one, every pack has the same bytes; every entry
 *   is byte-identical to v0.11.0's, and the manifest differs by the tool
 *   version. The ffmpeg step's child gets only its standard input, output
 *   and error: on Windows a PROC_THREAD_ATTRIBUTE_HANDLE_LIST, where every
 *   inheritable handle of the process went before (a player's log the
 *   designer was opening could stay open in ffmpeg); on POSIX
 *   posix_spawn_file_actions_addclosefrom_np(3) (glibc 2.34 and later;
 *   macOS POSIX_SPAWN_CLOEXEC_DEFAULT), the pipe close-on-exec, the
 *   default signal actions, and SCHED_OTHER.
 *
 *   v0.7.0 (2026-10-22): the definition's canonical bytes and its lookups
 *   (docs/pack.md 16). An experiment entry that is a definition ("format":
 *   "ysp-experiment") is stored as ysp/json.h's canonical form with one
 *   LF after it, derivation {"op": "canonical"} (experiment_format.md
 *   D2), not as the source's bytes; so definitions that differ only in
 *   whitespace, key order, number spelling or string escapes give one
 *   entry, one experiment ID and one pack ID. The source's "sha256" in the
 *   manifest is that of its canonical bytes too, and `ypak rebuild` checks
 *   a source so. Other experiment bytes are copied as before. A lookup
 *   whose map lacks a level its "by" can take (a table column's levels, a
 *   vars value or choice, a boolean built-in as far as the leaf's rules
 *   can make it true, outcome, response, method)
 *   is refused, naming the place, the key and the levels; map keys that
 *   no level reaches are a warning (ypt_options.warn;
 *   ypt_check_lookups()). Every entry of the test corpus is
 *   byte-identical to v0.6.0's (its experiment is opaque); a pack with a
 *   definition has a new experiment entry and pack ID.
 *
 *   v0.5.1 (2026-10-20): an ffmpeg step with mpeg2video is refused
 *   before it runs (the user's decision): the video index (pl_mpeg) reads
 *   MPEG-1 only and misread mpeg2video's output, so the step failed after
 *   the encode.
 *
 *   v0.5.0 (2026-10-20): the ffmpeg step runs on N threads, not one (the
 *   user's decision of 2026-10-20, docs/pack.md 14): N is the resource's
 *   "threads" (1 to 64) or the machine's logical processors, passed as
 *   `-threads N` (libx265: pools=N and x265's frame threads for N),
 *   recorded in the derivation, and used again by `ypak rebuild`. With N
 *   fixed, each encoder gave the same bytes under every CPU affinity and
 *   on Windows and Linux. The author's own thread settings stay refused.
 *   The tool also adds info=0 for libx265 and -flags +bitexact -dct int
 *   -idct simple for mpeg1video and mpeg2video, so their bytes do not
 *   depend on the CPU or the build (the user's decision of 2026-10-20),
 *   and refuses the author's contrary settings; libx264 has no such flag
 *   and rebuilds the same only on a CPU with its instruction-set path.
 *   A video entry made by ffmpeg differs from v0.4.0's (its command and
 *   the count); every other entry of the test corpus is byte-identical.
 *   ffmpeg's version line is refused, not cut, past 255 bytes (gcc's
 *   -Wformat-truncation stopped the Linux Release build).
 *
 *   Every call returns 0 or a negative code (ysp/pack.h's YPAK_ERR_*
 *   values) and writes one line into err: what failed, where, and what to
 *   do. Reports (verify, rebuild, list) go to the FILE* given; NULL is
 *   quiet. Nothing here keeps state between calls.
 *
 *   Determinism: the same source description, sources and tool version give
 *   the same bytes (docs/pack.md 7). The manifest records the tool and
 *   library versions but not the compiler, so packs from different builds
 *   of one version have one pack ID when their entries agree.
 */
#ifndef YSP_PACK_TOOL_H_INCLUDED
#define YSP_PACK_TOOL_H_INCLUDED

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "ysp/pack.h"

#ifdef __cplusplus
extern "C" {
#endif

#define YPT_VERSION_STRING "0.12.0"

/* A build that ypt_options.cancel stopped (beside ysp/pack.h's YPAK_ERR_*). */
#define YPT_ERR_CANCELED (-20)

typedef struct ypt_options {
    const char* sources;   /* the sources' folder; NULL: the description's folder */
    FILE*       log;       /* one line per entry built; NULL: quiet               */
    const char* ffmpeg;    /* the ffmpeg program; NULL: "ffmpeg" on the PATH     */
    FILE*       warn;      /* one line per warning ("warning: ..."); NULL: quiet */
    /* v0.12.0. Nonzero stops the build with YPT_ERR_CANCELED. Called on the
     * building thread before each resource and after an ffmpeg step (an
     * encode or a video index that runs goes to its end); a flag that
     * another thread sets must be read atomically here. NULL: never. */
    int       (*cancel)(void* ctx);
    void*       cancel_ctx;
    /* v0.12.0. The entry cache's folder (made, with its parents, when
     * missing), or NULL for none. Builds in several threads or processes
     * may share it. A record that does not read back whole is a miss, so
     * the pack's bytes never depend on the cache. ypt_rebuild_ex() takes
     * no cache: it derives every entry again. */
    const char* cache;
    /* v0.12.0. After a build that wrote a record, ypt_cache_trim() to this
     * many bytes; 0: no bound. */
    int64_t     cache_max;
} ypt_options;

/* Builds the pack that a source description (JSON, docs/pack.md 5.2)
 * describes and writes it to out_path (through a temporary file, so a
 * failed build leaves no pack). */
int ypt_build(const char* desc_path, const char* out_path, const ypt_options* o, char* err, size_t cap);
/* The same from description text in memory; base_dir holds the sources. */
int ypt_build_text(const char* json, size_t n, const char* base_dir, const char* out_path,
                   const ypt_options* o, char* err, size_t cap);

/* Every check of docs/pack.md 5.1 `ypak verify`: the reader's checks with
 * every entry hashed, CRC-32, SHA-256 against the manifest, the manifest
 * against the directory records, and the container against the canonical
 * one. A line per finding to report. 0 only when all pass. */
int ypt_verify(const char* pack_path, FILE* report, char* err, size_t cap);

/* Name, kind, size, data offset and flags of each entry. */
int ypt_list(const char* pack_path, FILE* out, char* err, size_t cap);
/* Pack ID, format, tool, experiment, project audio. */
int ypt_info(const char* pack_path, FILE* out, char* err, size_t cap);
/* Writes the named entries (all when n is 0) under dir, refusing names
 * outside the writer's rule and existing files unless force. */
int ypt_extract(const char* pack_path, const char* const* names, int n, const char* dir, int force,
                FILE* report, char* err, size_t cap);
/* One entry's bytes to out. */
int ypt_cat(const char* pack_path, const char* name, FILE* out, char* err, size_t cap);
/* Rebuilds the pack from its manifest and the sources under sources_dir,
 * writes it to out_path (NULL: a temporary file, removed after), and
 * compares it with the pack byte for byte. 0 only when identical; report
 * gets "identical" or the first entry and offset that differ. */
int ypt_rebuild(const char* pack_path, const char* sources_dir, const char* out_path, FILE* report,
                char* err, size_t cap);
/* The same with options (o->ffmpeg and o->cancel; o->sources, o->log and
 * o->cache are not used). An
 * ffmpeg step runs again only when this ffmpeg reports the version that the
 * manifest records; otherwise the entry's bytes come from the pack, checked
 * by its hashes, report says so, and everything made from it is rebuilt. */
int ypt_rebuild_ex(const char* pack_path, const char* sources_dir, const char* out_path, const ypt_options* o,
                   FILE* report, char* err, size_t cap);
/* Writes player, zero padding to a multiple of 4096, and the pack with
 * absolute offsets, to out_path. The pack ID does not change. */
int ypt_append(const char* player_path, const char* pack_path, const char* out_path, char* err, size_t cap);

/* --- the entry cache (docs/pack.md 19) --- */

/* Removes the least recently used records of the cache folder dir (a hit
 * makes a record the most recent) until their bytes total at most
 * max_bytes, and temporary files older than an hour (a build that ended
 * mid-write). A record another build holds open (Windows) stays. kept:
 * the bytes of records left (NULL: not wanted). 0, or a negative code
 * with err when the folder cannot be listed. */
int ypt_cache_trim(const char* dir, int64_t max_bytes, int64_t* kept, char* err, size_t cap);

/* A cache record's parts, as ypt_cache_parse() finds them in the bytes. */
typedef struct ypt_cache_part {
    uint32_t       flags;      /* the entry's flags (YPAK_F_*)                    */
    uint32_t       source;     /* 1: the entry's bytes are its source's; data NULL */
    const char*    deriv;      /* the derivation's JSON text, deriv_len bytes     */
    size_t         deriv_len;
    const uint8_t* data;       /* into the record's bytes                         */
    uint64_t       size;
} ypt_cache_part;
/* Checks a record's bytes (public for the fuzz target and tests): the
 * header, the key (NULL: not compared), the size, the XXH64 of everything
 * after the header, each part's bounds and zero padding. The number of
 * parts (1 to cap) into parts, or a negative code (YPAK_ERR_FORMAT,
 * YPAK_ERR_CORRUPT). The JSON is not parsed here. */
int ypt_cache_parse(const void* rec, size_t n, const uint8_t* key, ypt_cache_part* parts, int cap);

/* The writer's name rule (docs/pack.md 2.7): 1 when name is a portable
 * entry name. Case-insensitive uniqueness is the build's check. */
int ypt_name_ok(const char* name, size_t n);

/* --- source readers (pack/import.c), public for the fuzz target and tests --- */

/* One worksheet of an .xlsx workbook as RFC 4180 CSV (LF line ends) for
 * ysp/table.h: row 1 names the columns, and line k of the text is sheet row
 * k. A number keeps the text the workbook stores; a boolean is TRUE or
 * FALSE; a formula gives its saved value. Refused by name: a date or a
 * time, an error value, a formula without a saved value, a value in a
 * column without a name, a password, zip64, and a part past its bound. */
typedef struct ypt_sheet {
    char*    csv;          /* malloc'd; a NUL after csv_len                    */
    size_t   csv_len;
    char     sheet[256];   /* the sheet read                                   */
    int      n_cols;
    int      rows;         /* the last sheet row with a value, less the header */
    int      formulas;     /* cells whose value is a formula's saved result    */
    uint64_t text_cols;    /* bit k: every value of column k is a text cell    */
} ypt_sheet;
/* sheet: a sheet's name, or NULL for the first sheet. 0 or a negative code
 * with err. */
int  ypt_xlsx_read(const void* data, size_t n, const char* sheet, ypt_sheet* out, char* err, size_t cap);
void ypt_sheet_free(ypt_sheet* s);

/* A JPEG's frame and segments checked (baseline, extended or progressive
 * Huffman coding, 8-bit, 1 or 3 components, EXIF orientation 1 or none, an
 * ICC profile only when it is sRGB's), then, with decode, its texels from
 * stb_image: gray as 1 channel, color as RGBA with alpha 255. */
typedef struct ypt_jpeg {
    uint8_t* px;           /* w h channels bytes; ypt_jpeg_free() releases it */
    int      w, h, channels, components, progressive, icc_srgb;
    char     sampling[24]; /* "gray", or "2x2,1x1,1x1" (h x v per component)  */
} ypt_jpeg;
int  ypt_jpeg_read(const void* data, size_t n, int decode, ypt_jpeg* out, char* err, size_t cap);
void ypt_jpeg_free(ypt_jpeg* j);

/* 1 when an ICC profile is sRGB's (an RGB matrix-and-curves profile whose
 * colorants are within 0.002 of sRGB's D50 values and whose curves are
 * within half a code of 255 of sRGB's at every code), 0 when it is another
 * (why says how), < 0 when it is damaged. */
int ypt_icc_srgb(const void* data, size_t n, char* why, size_t cap);

/* --- the texts of an experiment definition (pack/derive.c), public for the
 * fuzz target and tests; the build calls it for the experiment entry --- */

/* One text that a `text` stimulus can show, and the stimulus's layout keys
 * (docs/pack.md 4.5). Numbers are their canonical decimal text
 * (ysp/json.h), as a glyph-run block takes them. Every string stays valid
 * until ypt_texts_free(). */
typedef struct ypt_text {
    const char* key;           /* ypak_runs_key() of style and text             */
    const char* text;          /* the key's last field                          */
    const char* stimulus;      /* the first stimulus, bytewise, that shows it   */
    const char* const* stimuli;  /* every one that does, sorted, each once      */
    int         n_stimuli;
    ypak_text_style style;     /* font (face 0), numbers, align, dir, lang      */
    const char* font;          /* style.font                                    */
    const char* size;          /* the key's spelling of style.size: above 0     */
    const char* width;         /* "0": no wrapping                              */
    const char* line_height;   /* "0": the font's own line                      */
    const char* align;         /* "start", "end" or "center"                    */
    const char* dir;           /* "auto", "ltr" or "rtl"                        */
    const char* lang;          /* BCP 47, or ""                                 */
} ypt_text;

typedef struct ypt_texts {
    int          definition;   /* 1: the bytes are a "ysp-experiment" definition */
    ypt_text*    texts;        /* sorted by key, each key once                   */
    int          n_texts;
    const char** tables;       /* the TABLE entries whose levels gave a text, sorted */
    int          n_tables;
    void*        mem_;
} ypt_texts;

/* The bytes of the pack's TABLE entry `name`, or NULL when there is none. */
typedef const void* (*ypt_table_fn)(void* ctx, const char* name, size_t* n);

/* Every text the definition's text stimuli can show (docs/experiment_format.md
 * D14, gap G9): a literal, a table column's levels, a node's vars, every
 * value of a lookup's map. Two stimuli of one style and one text give one
 * text, with both in stimuli. Bytes that are not JSON, or JSON whose "format"
 * is not "ysp-experiment", give 0 and definition 0. A text known only at run
 * time (a built-in or a session field named directly, a script, a variable
 * layout key) is refused, with err naming the stimulus. Call
 * ypt_texts_free() after a 0. */
int  ypt_derive_texts(const void* def, size_t n, ypt_table_fn table, void* ctx, ypt_texts* out, char* err, size_t cap);
void ypt_texts_free(ypt_texts* t);

/* One warning line, without a newline. */
typedef void (*ypt_warn_fn)(void* ctx, const char* line);

/* Every lookup {"by", "map"} in the definition's stimuli, templates and
 * nodes' vars and data against the levels its "by" can take in each leaf
 * node that reaches it (docs/experiment_format.md 2.2): a table column's
 * levels (a number as its canonical text), a vars value or each choice, a
 * boolean built-in ("false"; "true" too, except for practice, warmup and
 * after_break when the leaf's rules make no such trial), outcome ("correct", "wrong", and
 * "timeout" when the response has a deadline), response (the choices'
 * keys), method (the tracks). A level without a key gives YPAK_ERR_FORMAT,
 * with err naming the stimulus, template or node, the key, the levels and
 * the leaf. Keys that no level reaches go to warn (NULL: not computed),
 * one line per lookup. A "by" whose levels are known only at run time is
 * not checked. Bytes that are not a version 1 definition give 0. */
int  ypt_check_lookups(const void* def, size_t n, ypt_table_fn table, void* ctx, ypt_warn_fn warn, void* wctx, char* err, size_t cap);

#ifdef __cplusplus
}
#endif
#endif
