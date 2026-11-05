/* import_test.c - self-checking test of the pack tool's readers of other
 * formats (pack/import.c): the .xlsx reader (zip, inflate, XML, shared
 * strings, styles, the sheet into CSV), the JPEG reader (segments, EXIF,
 * ICC chunks, frame header, stb_image's decode) and the ICC profile check.
 * Every file is written here (tests/adapt/import_gen.h). Each refusal is
 * checked by its message. No framework: it returns 0 when every check
 * passed and 1 after printing each failure.
 *
 * It compiles pack/import.c into itself, so the mutation runner can mutate
 * that file (tests/mutate/import.toml), and links lodepng and stb_image
 * (CMake: test_pack_import), or with YPT_TEST_UNITY compiles them in too:
 *
 *     gcc -std=c11 -O1 -Wall -Wextra -DYPT_TEST_UNITY -Iinclude -Ipack
 *         -Ithird_party/lodepng -Ithird_party/stb tests/adapt/import_test.c -lm
 */
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif
#define YSP_PACK_IMPLEMENTATION
#define YPAK_NO_FILE
#include "ysp/pack.h"
#define YSP_TABLE_IMPLEMENTATION
#include "ysp/table.h"
#ifdef YPT_TEST_UNITY
#define LODEPNG_NO_COMPILE_CPP
#define LODEPNG_NO_COMPILE_ENCODER
#define LODEPNG_NO_COMPILE_DISK
#include "lodepng.cpp"
#include "stb_image.c"
#endif
#include "import.c"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "import_gen.h"

static int g_fail, g_checks;
#define CHECK(c, ...) do { g_checks++; if (!(c)) { g_fail++; printf("FAIL line %d: ", __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

/* --- xlsx -------------------------------------------------------------------- */

static int sheet_of(const char* sheet1, const char* want, ypt_sheet* s, char* err, size_t cap) {
    bb b;
    int rc;
    make_xlsx(&b, sheet1);
    rc = ypt_xlsx_read(b.d, b.n, want, s, err, cap);
    free(b.d);
    return rc;
}

/* A sheet made of the header row and these cells must be refused with says. */
static void sheet_refused(const char* cells, const char* says) {
    char sheet[8192], err[512];
    ypt_sheet s;
    int rc;
    snprintf(sheet, sizeof sheet, "%s%s</sheetData></worksheet>", XL_SHEET_HEAD, cells);
    rc = sheet_of(sheet, NULL, &s, err, sizeof err);
    CHECK(rc < 0 && strstr(err, says) && !s.csv, "refused '%s': rc %d, '%s'", says, rc, err);
}

/* The zip of make_xlsx() changed by edit(), then read. */
static void zip_refused(void (*edit)(bb*), const char* says) {
    bb b;
    ypt_sheet s;
    char err[512];
    int rc;
    make_xlsx(&b, g_sheet1);
    edit(&b);
    rc = ypt_xlsx_read(b.d, b.n, NULL, &s, err, sizeof err);
    CHECK(rc < 0 && strstr(err, says), "zip refused '%s': rc %d, '%s'", says, rc, err);
    free(b.d);
}

static size_t find_bytes(const bb* b, const void* what, size_t n, size_t from) {
    size_t i;
    for (i = from; i + n <= b->n; i++)
        if (!memcmp(b->d + i, what, n)) return i;
    return (size_t)-1;
}
static size_t eocd(const bb* b) { return b->n - 22; }
static size_t central(const bb* b, const char* name) {
    size_t at = (size_t)b->d[eocd(b) + 16] | (size_t)b->d[eocd(b) + 17] << 8 | (size_t)b->d[eocd(b) + 18] << 16;
    size_t k = find_bytes(b, name, strlen(name), at);
    return k == (size_t)-1 ? k : k - 46;
}
static void put32(uint8_t* p, uint32_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24); }

static void e_ole(bb* b) { static const uint8_t ole[8] = { 0xD0, 0xCF, 0x11, 0xE0, 0xA1, 0xB1, 0x1A, 0xE1 }; memcpy(b->d, ole, 8); }
static void e_no_eocd(bb* b) { b->d[eocd(b)] = 'X'; }
static void e_zip64(bb* b) { put32(b->d + eocd(b) + 16, 0xFFFFFFFFu); }
static void e_cd_out(bb* b) { put32(b->d + eocd(b) + 12, 0x7FFFFFF0u); }
static void e_encrypted(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); b->d[c + 8] |= 1; }
static void e_method(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); b->d[c + 10] = 12; }
static void e_big(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); put32(b->d + c + 24, 100u << 20); }
static void e_usize(bb* b) {
    size_t c = central(b, "xl/worksheets/sheet1.xml");
    uint32_t u = (uint32_t)b->d[c + 24] | (uint32_t)b->d[c + 25] << 8 | (uint32_t)b->d[c + 26] << 16 | (uint32_t)b->d[c + 27] << 24;
    put32(b->d + c + 24, u + 1);
}
static void e_crc(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); b->d[c + 16] ^= 1; }
static void e_lho(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); put32(b->d + c + 42, (uint32_t)b->n); }
static void e_csize(bb* b) { size_t c = central(b, "xl/worksheets/sheet1.xml"); put32(b->d + c + 20, 0x7FFFFFF0u); }
static void e_stored_sizes(bb* b) { size_t c = central(b, "xl/styles.xml"); b->d[c + 20] ^= 1; }
static void e_no_rels(bb* b) { size_t k = find_bytes(b, "xl/_rels/workbook.xml.rels", 26, 0); while (k != (size_t)-1) { b->d[k + 3] = 'Q'; k = find_bytes(b, "xl/_rels/workbook.xml.rels", 26, k + 1); } }
static void e_no_sheet(bb* b) { size_t k = find_bytes(b, "xl/worksheets/sheet1.xml", 24, 0); while (k != (size_t)-1) { b->d[k + 14] = 'Q'; k = find_bytes(b, "xl/worksheets/sheet1.xml", 24, k + 1); } }

static void test_xlsx(void) {
    static const char want[] =
        "word,contrast,code,flag,note\n"
        "cat,0.25,007,TRUE,\"a, \"\"b\"\"\"\n"
        "d<og,1,xAy,FALSE,\n"
        ",,,,\n"
        "\xce\xb1\xce\xb2,1.0000000000000001E-2,007,FALSE,xy\n";
    ypt_sheet s;
    char err[512];
    int rc = sheet_of(g_sheet1, NULL, &s, err, sizeof err);
    CHECK(rc == 0, "the workbook: %s", err);
    if (rc == 0) {
        CHECK(s.csv_len == strlen(want) && !memcmp(s.csv, want, s.csv_len), "the CSV:\n%s", s.csv);
        CHECK(!strcmp(s.sheet, "Conditions") && s.n_cols == 5 && s.rows == 4 && s.formulas == 2 && s.text_cols == 0x15u,
              "sheet '%s', %d columns, %d rows, %d formulas, text columns 0x%x", s.sheet, s.n_cols, s.rows, s.formulas, (unsigned)s.text_cols);
        ypt_sheet_free(&s);
        CHECK(!s.csv && !s.csv_len, "ypt_sheet_free");
    }
    rc = sheet_of(g_sheet1, "Sheet2", &s, err, sizeof err);
    CHECK(rc == 0 && !strcmp(s.csv, "n\n3\n") && !strcmp(s.sheet, "Sheet2"), "the sheet named Sheet2 (absolute target): %s", rc ? err : s.csv);
    ypt_sheet_free(&s);
    rc = sheet_of(g_sheet1, "Nope", &s, err, sizeof err);
    CHECK(rc < 0 && strstr(err, "no worksheet named 'Nope'; the workbook has 'Conditions', 'Sheet2'"), "a missing sheet: %s", err);

    /* cells */
    sheet_refused("<row r=\"2\"><c r=\"B2\" s=\"1\"><v>45000</v></c></row>", "cell B2 is a date or a time");
    sheet_refused("<row r=\"2\"><c r=\"B2\" s=\"2\"><v>45000</v></c></row>", "cell B2 is a date or a time");
    sheet_refused("<row r=\"2\"><c r=\"B2\"><f>1+1</f></c></row>", "cell B2 has a formula and no saved value");
    sheet_refused("<row r=\"2\"><c r=\"B2\" t=\"e\"><v>#N/A</v></c></row>", "cell B2 holds the error #N/A");
    sheet_refused("<row r=\"2\"><c r=\"B2\" t=\"d\"><v>2026-10-13</v></c></row>", "cell B2 is a date");
    sheet_refused("<row r=\"2\"><c r=\"B2\" t=\"zz\"><v>1</v></c></row>", "unknown type 'zz'");
    sheet_refused("<row r=\"2\"><c r=\"B2\"><v>1,5</v></c></row>", "'1,5' is not a number");
    sheet_refused("<row r=\"2\"><c r=\"B2\" t=\"b\"><v>2</v></c></row>", "a boolean that is not 0 or 1");
    sheet_refused("<row r=\"2\"><c r=\"B2\" t=\"s\"><v>9</v></c></row>", "shared string '9'");
    sheet_refused("<row r=\"2\"><c r=\"G2\"><v>1</v></c></row>", "cell G2 has a value, but its column has no name in row 1");
    sheet_refused("<row r=\"2\"><c r=\"B2\"><v>1</v></c><c r=\"A2\"><v>1</v></c></row>", "out of order");
    sheet_refused("<row r=\"3\"/><row r=\"2\"/>", "row number '2' out of order");
    sheet_refused("<row r=\"2\"/><row r=\"2\"/>", "row number '2' out of order");
    sheet_refused("<row r=\"2\"><c r=\"B3\"><v>1</v></c></row>", "cell reference 'B3'");
    sheet_refused("<row r=\"2\"><c r=\"XFE2\"><v>1</v></c></row>", "cell reference 'XFE2'");
    sheet_refused("<row r=\"40000\"><c r=\"A40000\"><v>1</v></c></row>", "values down to row 40000");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>&bogus;</t></is></c></row>", "'&bogus;' is not one of XML's five");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>&#0;</t></is></c></row>", "U+0000");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>&#xD800;</t></is></c></row>", "U+D800");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>_xD800_</t></is></c></row>", "escape to U+D800");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>a</t></is></c></row><!DOCTYPE x>", "DOCTYPE");
    sheet_refused("<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>a</t></is></c></row><!-- open", "a comment without its end");
    sheet_refused("<row r=\"2\"><c r=\"A2\"", "'<' inside a tag");
    {
        char sheet[8192];
        int i;
        /* a field longer than ysp/table.h takes */
        strcpy(sheet, "<row r=\"2\"><c r=\"A2\" t=\"inlineStr\"><is><t>");
        for (i = 0; i < 5000; i++) strcat(sheet, "x");
        strcat(sheet, "</t></is></c></row>");
        sheet_refused(sheet, "cell A2 holds 5000 bytes");
    }
    {
        /* row 1 empty; a number in row 1 */
        char err2[512];
        ypt_sheet s2;
        CHECK(sheet_of("<worksheet><sheetData><row r=\"2\"><c r=\"A2\"><v>1</v></c></row></sheetData></worksheet>", NULL, &s2, err2, sizeof err2) < 0 &&
              strstr(err2, "row 1 is empty"), "row 1 empty: %s", err2);
        CHECK(sheet_of("<worksheet><sheetData><row r=\"1\"><c r=\"A1\"><v>1</v></c></row></sheetData></worksheet>", NULL, &s2, err2, sizeof err2) < 0 &&
              strstr(err2, "cell A1 in row 1 is not text"), "a number in row 1: %s", err2);
        CHECK(sheet_of("<worksheet><sheetData></sheetData></worksheet>", NULL, &s2, err2, sizeof err2) < 0 && strstr(err2, "the sheet is empty"), "empty: %s", err2);
        CHECK(sheet_of("<worksheet></worksheet>", NULL, &s2, err2, sizeof err2) < 0 && strstr(err2, "no sheetData"), "no sheetData: %s", err2);
        /* CDATA, comments, processing instructions; cells without r */
        CHECK(sheet_of("<?xml version=\"1.0\"?><!-- c --><worksheet><sheetData><row><c t=\"inlineStr\"><is><t><![CDATA[a&b]]></t></is></c>"
                       "<c t=\"inlineStr\"><is><t>b</t></is></c></row><row><c><v>1</v></c><c><v>-2.5e3</v></c></row></sheetData></worksheet>",
                       NULL, &s2, err2, sizeof err2) == 0 && !strcmp(s2.csv, "a&b,b\n1,-2.5e3\n"), "CDATA and implicit references: %s", s2.csv ? s2.csv : err2);
        ypt_sheet_free(&s2);
        /* a comma alone is quoted; a column of numbers and text is not a
         * text column; a column of text only is */
        CHECK(sheet_of("<worksheet><sheetData><row><c t=\"inlineStr\"><is><t>a</t></is></c><c t=\"inlineStr\"><is><t>b</t></is></c></row>"
                       "<row><c t=\"inlineStr\"><is><t>x,y</t></is></c><c><v>1</v></c></row>"
                       "<row><c t=\"inlineStr\"><is><t>z</t></is></c><c t=\"inlineStr\"><is><t>w</t></is></c></row></sheetData></worksheet>",
                       NULL, &s2, err2, sizeof err2) == 0 && !strcmp(s2.csv, "a,b\n\"x,y\",1\nz,w\n") && s2.text_cols == 1u,
              "quoting and text columns: %s (0x%x)", s2.csv ? s2.csv : err2, (unsigned)s2.text_cols);
        ypt_sheet_free(&s2);
        /* x: prefixed SpreadsheetML */
        CHECK(sheet_of("<x:worksheet xmlns:x=\"u\"><x:sheetData><x:row r=\"1\"><x:c r=\"A1\" t=\"inlineStr\"><x:is><x:t>q</x:t></x:is></x:c></x:row>"
                       "<x:row r=\"2\"><x:c r=\"A2\"><x:v>7</x:v></x:c></x:row></x:sheetData></x:worksheet>", NULL, &s2, err2, sizeof err2) == 0 &&
              !strcmp(s2.csv, "q\n7\n"), "prefixed element names: %s", s2.csv ? s2.csv : err2);
        ypt_sheet_free(&s2);
    }

    /* the zip */
    zip_refused(e_ole, "password-protected");
    zip_refused(e_no_eocd, "no end of central directory");
    zip_refused(e_zip64, "zip64");
    zip_refused(e_cd_out, "outside the file");
    zip_refused(e_encrypted, "is encrypted");
    zip_refused(e_method, "compression method 12");
    zip_refused(e_big, "the limit is 64 MiB");
    zip_refused(e_usize, "inflates to");
    zip_refused(e_crc, "fails its CRC-32");
    zip_refused(e_lho, "no local header");
    zip_refused(e_csize, "runs past the end");
    zip_refused(e_stored_sizes, "stored with two sizes");
    zip_refused(e_no_rels, "not a workbook");
    zip_refused(e_no_sheet, "the sheet's part");
    {
        char err2[256];
        ypt_sheet s2;
        CHECK(ypt_xlsx_read("PK", 2, NULL, &s2, err2, sizeof err2) < 0 && strstr(err2, "not a zip"), "two bytes: %s", err2);
    }

    /* every truncation of the workbook is refused or read, never more */
    {
        bb b;
        size_t n, refused = 0;
        make_xlsx(&b, g_sheet1);
        for (n = 0; n < b.n; n++) {
            ypt_sheet s2;
            char err2[256];
            if (ypt_xlsx_read(b.d, n, NULL, &s2, err2, sizeof err2) < 0) refused++;
            ypt_sheet_free(&s2);
        }
        CHECK(refused == b.n, "truncations: %zu of %zu refused", refused, b.n);
        free(b.d);
    }
}

/* --- ICC ----------------------------------------------------------------------- */

static void test_icc(void) {
    bb p;
    char why[256];
    memset(&p, 0, sizeof p);
    make_icc(&p, 1);
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 1, "sRGB profile: %s", why);
    CHECK(ypt_icc_srgb(p.d, 100, why, sizeof why) < 0, "a short profile is damaged");
    p.d[3] = (uint8_t)(p.d[3] + 4);   /* the size past the bytes */
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) < 0 && strstr(why, "damaged"), "size past the end: %s", why);
    p.d[3] = (uint8_t)(p.d[3] - 4);
    p.d[132 + 4] = 0x7F;              /* rXYZ's offset out of range */
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) < 0 && strstr(why, "rXYZ"), "a tag out of range: %s", why);
    free(p.d);
    memset(&p, 0, sizeof p);
    make_icc(&p, 0);
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 0 && strstr(why, "primaries are not sRGB's"), "a panel's profile: %s", why);
    free(p.d);
    memset(&p, 0, sizeof p);
    make_icc(&p, 1);
    /* sRGB's colorants with a pure gamma of 2.2: refused by its curve */
    memcpy(p.d + 264, "curv\0\0\0\0\0\0\0\x01\x02\x33\0\0", 16);
    p.d[132 + 12 * 3 + 11] = 16; p.d[132 + 12 * 4 + 11] = 16; p.d[132 + 12 * 5 + 11] = 16;
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 0 && strstr(why, "rTRC is not sRGB's curve"), "a pure gamma: %s", why);
    /* a 2-point table: linear, far from sRGB */
    memcpy(p.d + 264, "curv\0\0\0\0\0\0\0\x02\0\0\xff\xff", 16);
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 0 && strstr(why, "codes of 255 from sRGB's curve"), "a linear table: %s", why);
    /* para type 0 (gamma 2.4 alone) */
    memcpy(p.d + 264, "para\0\0\0\0\0\0\0\0\0\x02\x66\x66", 16);
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 0 && strstr(why, "from sRGB's curve"), "para gamma 2.4: %s", why);
    memcpy(p.d + 16, "GRAY", 4);
    CHECK(ypt_icc_srgb(p.d, p.n, why, sizeof why) == 0 && strstr(why, "not RGB"), "a gray profile: %s", why);
    free(p.d);
}

/* --- JPEG ------------------------------------------------------------------------ */

static int jpeg_of(int w, int h, int nc, int sub, int marker, int prec, int flat, const bb* app, int decode, ypt_jpeg* j, char* err, size_t cap) {
    bb b;
    int rc;
    make_jpeg(&b, w, h, nc, sub, marker, prec, flat, app);
    rc = ypt_jpeg_read(b.d, b.n, decode, j, err, cap);
    free(b.d);
    return rc;
}

static void jpeg_refused(const char* label, int nc, int marker, int prec, const bb* app, const char* says) {
    ypt_jpeg j;
    char err[512];
    int rc = jpeg_of(16, 16, nc, 1, marker, prec, 0, app, 1, &j, err, sizeof err);
    CHECK(rc < 0 && strstr(err, says) && !j.px, "%s: rc %d, '%s'", label, rc, err);
}

static void test_jpeg(void) {
    ypt_jpeg j;
    char err[512];
    bb app;
    int rc, bad, k;
    /* gray, baseline: every texel its block's value */
    rc = jpeg_of(20, 12, 1, 1, 0xC0, 8, 0, NULL, 1, &j, err, sizeof err);
    CHECK(rc == 0 && j.w == 20 && j.h == 12 && j.channels == 1 && j.components == 1 && !j.progressive && !strcmp(j.sampling, "gray"), "gray: %s", err);
    for (k = 0, bad = 0; rc == 0 && k < 20 * 12; k++) bad += j.px[k] != jpeg_val(0, (k % 20) / 8, (k / 20) / 8, 0);
    CHECK(bad == 0, "gray texels: %d differ", bad);
    ypt_jpeg_free(&j);
    CHECK(!j.px, "ypt_jpeg_free");
    /* progressive, no chroma: R = G = B = Y, alpha 255 */
    rc = jpeg_of(24, 16, 3, 1, 0xC2, 8, 1, NULL, 1, &j, err, sizeof err);
    CHECK(rc == 0 && j.channels == 4 && j.progressive && !strcmp(j.sampling, "1x1,1x1,1x1"), "progressive: %s", err);
    for (k = 0, bad = 0; rc == 0 && k < 24 * 16; k++) {
        int v = jpeg_val(0, (k % 24) / 8, (k / 24) / 8, 1);
        bad += j.px[4 * k] != v || j.px[4 * k + 1] != v || j.px[4 * k + 2] != v || j.px[4 * k + 3] != 255;
    }
    CHECK(bad == 0, "progressive texels: %d differ", bad);
    ypt_jpeg_free(&j);
    /* 4:2:0 with EXIF orientation 1 and an sRGB profile in three chunks; header only */
    memset(&app, 0, sizeof app);
    app_exif(&app, 1);
    app_icc(&app, 1, 3);
    rc = jpeg_of(40, 24, 3, 2, 0xC0, 8, 0, &app, 0, &j, err, sizeof err);
    CHECK(rc == 0 && !j.px && j.w == 40 && j.icc_srgb && !strcmp(j.sampling, "2x2,1x1,1x1"), "4:2:0 header only: %s", err);
    rc = jpeg_of(40, 24, 3, 2, 0xC0, 8, 0, &app, 1, &j, err, sizeof err);
    CHECK(rc == 0 && j.px && j.px[3] == 255, "4:2:0 decoded: %s", err);
    ypt_jpeg_free(&j);
    free(app.d);
    /* refusals */
    memset(&app, 0, sizeof app);
    app_exif(&app, 6);
    jpeg_refused("orientation 6", 1, 0xC0, 8, &app, "EXIF orientation is 6");
    app.n = 0;
    app_exif(&app, 8);
    jpeg_refused("orientation 8", 1, 0xC0, 8, &app, "EXIF orientation is 8");
    app.n = 0;
    app_icc(&app, 0, 1);
    jpeg_refused("a panel's profile", 3, 0xC0, 8, &app, "primaries are not sRGB's");
    app.n = 0;
    app_icc(&app, 1, 2);
    app.d[17] = 3;   /* the first chunk says 3 chunks */
    jpeg_refused("chunk counts disagree", 1, 0xC0, 8, &app, "damaged ICC profile chunks");
    app.n = 0;
    app_icc(&app, 1, 2);
    {
        /* chunk numbers swapped; a segment is its marker and its length */
        size_t second = 2 + ((size_t)app.d[2] << 8 | app.d[3]);
        app.d[16] = 2;
        app.d[second + 16] = 1;
    }
    jpeg_refused("chunks out of order", 1, 0xC0, 8, &app, "out of order");
    app.n = 0;
    app_icc(&app, 1, 2);
    app.n = 2 + ((size_t)app.d[2] << 8 | app.d[3]);   /* the first chunk only */
    jpeg_refused("a missing chunk", 1, 0xC0, 8, &app, "1 of 2 ICC profile chunks");
    app.n = 0;
    seg(&app, 0xE1, (const uint8_t*)"Exif\0\0II*\0\xff\xff\xff\x7f", 14);
    jpeg_refused("damaged EXIF", 1, 0xC0, 8, &app, "damaged EXIF");
    free(app.d);
    jpeg_refused("CMYK", 4, 0xC0, 8, NULL, "CMYK");
    jpeg_refused("arithmetic", 1, 0xC9, 8, NULL, "SOF9 (arithmetic coding)");
    jpeg_refused("lossless", 1, 0xC3, 8, NULL, "SOF3 (lossless)");
    jpeg_refused("hierarchical", 1, 0xC5, 8, NULL, "SOF5 (hierarchical)");
    jpeg_refused("12-bit", 1, 0xC1, 12, NULL, "12-bit samples");
    jpeg_refused("2 components", 2, 0xC0, 8, NULL, "2 components");
    {
        bb b;
        ypt_jpeg j2;
        size_t sos;
        make_jpeg(&b, 16, 16, 1, 1, 0xC0, 8, 0, NULL);
        CHECK(ypt_jpeg_read(b.d + 2, b.n - 2, 1, &j2, err, sizeof err) < 0 && strstr(err, "no SOI"), "no SOI: %s", err);
        sos = find_bytes(&b, "\xff\xda", 2, 0);
        CHECK(ypt_jpeg_read(b.d, sos, 1, &j2, err, sizeof err) < 0 && strstr(err, "ends before its image data"), "truncated before SOS: %s", err);
        /* the height 0: a DNL marker would give it */
        b.d[find_bytes(&b, "\xff\xc0", 2, 0) + 5] = 0;
        b.d[find_bytes(&b, "\xff\xc0", 2, 0) + 6] = 0;
        CHECK(ypt_jpeg_read(b.d, b.n, 1, &j2, err, sizeof err) < 0 && strstr(err, "DNL"), "height 0: %s", err);
        free(b.d);
        /* a frame far larger than the data: refused before stb_image pads it */
        make_jpeg(&b, 16, 16, 1, 1, 0xC0, 8, 0, NULL);
        {
            size_t f = find_bytes(&b, "\xff\xc0", 2, 0);
            b.d[f + 5] = 0x10; b.d[f + 6] = 0x00; b.d[f + 7] = 0x10; b.d[f + 8] = 0x00;   /* 4096 x 4096 */
            CHECK(ypt_jpeg_read(b.d, b.n, 1, &j2, err, sizeof err) < 0 && strstr(err, "too short for its size: 4096 x 4096 needs 262144 blocks"),
                  "a large frame on little data: %s", err);
        }
        free(b.d);
        /* the frame header twice */
        make_jpeg(&b, 16, 16, 1, 1, 0xC0, 8, 0, NULL);
        {
            size_t f = find_bytes(&b, "\xff\xc0", 2, 0), n = 2 + ((size_t)b.d[f + 2] << 8 | b.d[f + 3]);
            bb c;
            memset(&c, 0, sizeof c);
            bput(&c, b.d, f + n);
            bput(&c, b.d + f, b.n - f);
            CHECK(ypt_jpeg_read(c.d, c.n, 1, &j2, err, sizeof err) < 0 && strstr(err, "two frame headers"), "two SOFs: %s", err);
            free(c.d);
        }
        /* entropy data cut short: no end marker, refused before stb_image
         * would pad the missing data with zeros */
        {
            size_t cut, refused = 0;
            for (cut = sos; cut < b.n; cut++) {
                if (ypt_jpeg_read(b.d, cut, 1, &j2, err, sizeof err) < 0) refused++;
                ypt_jpeg_free(&j2);
            }
            CHECK(refused == b.n - sos, "cut entropy data: %zu of %zu refused", refused, b.n - sos);
            CHECK(ypt_jpeg_read(b.d, b.n - 1, 1, &j2, err, sizeof err) < 0 && strstr(err, "no end marker"), "no EOI: %s", err);
        }
        free(b.d);
    }
}

int main(void) {
    test_xlsx();
    test_icc();
    test_jpeg();
    printf("import_test: %d checks, %d failed\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
