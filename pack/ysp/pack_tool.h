/* ysp/pack_tool.h - v0.4.0 - the pack tool as a library (prefix ypt_)
 *
 *   Builds, verifies, lists, extracts, rebuilds and appends ysp packs
 *   (docs/pack.md). The CLI `ypak` (pack/ypak.c) is a thin layer over these
 *   calls, and the designer runs the same code as WebAssembly. Not a single
 *   header: a C11 library in the pack tool's tree that links ysp/pack.h,
 *   ysp/json.h (every description and manifest), ysp/table.h,
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
 *   step whose argument vector and version the manifest records; FRAMESEQ
 *   from a ysp/video.h frame sequence, every frame checked; a TABLE from an
 *   .xlsx sheet; a TEXTURE from a JPEG; an ICC profile in a PNG or JPEG is
 *   accepted when it is sRGB's and refused otherwise (v0.3.0 refused every
 *   iCCP). `ypak rebuild` takes an ffmpeg output from the pack when this
 *   ffmpeg is another version. The manifest names ysp/video.h, stb_image
 *   and pl_mpeg, so every pack ID differs from v0.3.0's; every other entry
 *   of the v0.3.0 test corpus is byte-identical.
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

#ifdef __cplusplus
extern "C" {
#endif

#define YPT_VERSION_STRING "0.4.0"

typedef struct ypt_options {
    const char* sources;   /* the sources' folder; NULL: the description's folder */
    FILE*       log;       /* one line per entry built; NULL: quiet               */
    const char* ffmpeg;    /* the ffmpeg program; NULL: "ffmpeg" on the PATH     */
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
/* The same with options (o->ffmpeg; o->sources and o->log are not used). An
 * ffmpeg step runs again only when this ffmpeg reports the version that the
 * manifest records; otherwise the entry's bytes come from the pack, checked
 * by its hashes, report says so, and everything made from it is rebuilt. */
int ypt_rebuild_ex(const char* pack_path, const char* sources_dir, const char* out_path, const ypt_options* o,
                   FILE* report, char* err, size_t cap);
/* Writes player, zero padding to a multiple of 4096, and the pack with
 * absolute offsets, to out_path. The pack ID does not change. */
int ypt_append(const char* player_path, const char* pack_path, const char* out_path, char* err, size_t cap);

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

#ifdef __cplusplus
}
#endif
#endif
