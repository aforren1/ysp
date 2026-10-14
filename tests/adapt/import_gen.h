/* import_gen.h - writers of the source files the pack tool's readers take
 * (pack/import.c): ICC profiles, JPEG files with every 8x8 block flat, and
 * .xlsx workbooks with stored and fixed-Huffman deflated parts. Integer
 * arithmetic only, so every platform writes the same bytes, and nothing to
 * link. Shared by tests/adapt/import_test.c and tests/pack/tool_test.c;
 * include ysp/pack.h (for ypak_crc32) and <stdlib.h>, <string.h>,
 * <stdio.h> first. */
#ifndef YSP_IMPORT_GEN_H
#define YSP_IMPORT_GEN_H

typedef struct bb { uint8_t* d; size_t n, cap; } bb;
static void b_need(bb* b, size_t k) {
    if (b->n + k > b->cap) {
        while (b->n + k > b->cap) b->cap = b->cap ? 2 * b->cap : 256;
        b->d = (uint8_t*)realloc(b->d, b->cap);
        if (!b->d) { printf("out of memory\n"); exit(1); }
    }
}
static void b8(bb* b, uint32_t v) { b_need(b, 1); b->d[b->n++] = (uint8_t)v; }
static void b16(bb* b, uint32_t v) { b8(b, v >> 8 & 255); b8(b, v & 255); }
static void b32(bb* b, uint32_t v) { b16(b, v >> 16); b16(b, v & 0xffff); }
static void bput(bb* b, const void* p, size_t n) { b_need(b, n); memcpy(b->d + b->n, p, n); b->n += n; }
static void le16(bb* b, uint32_t v) { b8(b, v & 255); b8(b, v >> 8 & 255); }
static void le32(bb* b, uint32_t v) { le16(b, v & 0xffff); le16(b, v >> 16); }


/* An ICC v2 RGB matrix-and-curves profile: sRGB's colorants and its curve
 * as a parametric curve (type 3), or a display's (a laptop panel's
 * colorants and a pure gamma of 1.8), which the tool must refuse. Integer
 * constants only, so every platform writes the same bytes. */
static void make_icc(bb* b, int srgb) {
    static const int32_t s_xyz[3][3] = { { 28580, 14582, 911 }, { 25238, 46983, 6364 }, { 9378, 3971, 46799 } };
    static const int32_t d_xyz[3][3] = { { 25880, 14752, 1940 }, { 28043, 45358, 9450 }, { 9260, 5459, 42664 } };
    static const char* const tags[6] = { "rXYZ", "gXYZ", "bXYZ", "rTRC", "gTRC", "bTRC" };
    size_t start = b->n, k;
    uint32_t trc_off = 132 + 6 * 12 + 3 * 20, trc_n = srgb ? 32 : 16;
    b32(b, 0); b32(b, 0); b32(b, 0x02100000u); bput(b, "mntr", 4); bput(b, "RGB ", 4); bput(b, "XYZ ", 4);
    for (k = 0; k < 12; k++) b8(b, 0);
    bput(b, "acsp", 4);
    for (k = 0; k < 24; k++) b8(b, 0);   /* platform, flags, maker, model, attributes */
    b32(b, 0);                             /* intent */
    b32(b, 63190); b32(b, 65536); b32(b, 54061);   /* D50 */
    for (k = 0; k < 48; k++) b8(b, 0);   /* creator, id, reserved */
    b32(b, 6);
    for (k = 0; k < 6; k++) {
        bput(b, tags[k], 4);
        b32(b, k < 3 ? 132 + 6 * 12 + 20 * (uint32_t)k : trc_off);
        b32(b, k < 3 ? 20 : trc_n);
    }
    for (k = 0; k < 3; k++) {
        const int32_t* v = srgb ? s_xyz[k] : d_xyz[k];
        bput(b, "XYZ ", 4); b32(b, 0); b32(b, (uint32_t)v[0]); b32(b, (uint32_t)v[1]); b32(b, (uint32_t)v[2]);
    }
    if (srgb) {   /* 2.4, 1/1.055, 0.055/1.055, 1/12.92, 0.04045 */
        bput(b, "para", 4); b32(b, 0); b16(b, 3); b16(b, 0);
        b32(b, 157286); b32(b, 62119); b32(b, 3417); b32(b, 5072); b32(b, 2651);
    } else {      /* gamma 1.8 */
        bput(b, "curv", 4); b32(b, 0); b32(b, 1); b16(b, 0x01CD); b16(b, 0);
    }
    k = b->n - start;
    b->d[start] = (uint8_t)(k >> 24); b->d[start + 1] = (uint8_t)(k >> 16); b->d[start + 2] = (uint8_t)(k >> 8); b->d[start + 3] = (uint8_t)k;
}

/* --- JPEG files: every 8x8 block flat (DC only, quantizer 1), written with
 * integer arithmetic, so every platform makes the same bytes -------------- */

typedef struct bitw { bb* b; uint32_t acc; int n; } bitw;
static void bw_put(bitw* w, uint32_t code, int len) {   /* MSB first, 0xFF stuffed */
    int i;
    for (i = len - 1; i >= 0; i--) {
        w->acc = (w->acc << 1) | ((code >> i) & 1u);
        if (++w->n == 8) {
            b8(w->b, w->acc & 255);
            if ((w->acc & 255) == 255) b8(w->b, 0);
            w->acc = 0;
            w->n = 0;
        }
    }
}
static void bw_flush(bitw* w) { while (w->n) bw_put(w, 1, 1); }

/* the standard luminance DC table (ITU T.81 K.3) */
static const uint8_t dc_bits[16] = { 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0 };
static void put_dc(bitw* w, int diff) {
    uint32_t code = 0;
    int a = diff < 0 ? -diff : diff, s = 0, len, k, sym = 0;
    while (a >> s) s++;
    for (len = 1; len <= 16; len++) {
        for (k = 0; k < dc_bits[len - 1]; k++, sym++, code++)
            if (sym == s) {
                bw_put(w, code, len);
                if (s) bw_put(w, (uint32_t)(diff < 0 ? diff - 1 : diff) & ((1u << s) - 1u), s);
                return;
            }
        code <<= 1;
    }
}

static void seg(bb* b, int marker, const uint8_t* d, size_t n) {
    b8(b, 0xFF); b8(b, (uint32_t)marker); b16(b, (uint32_t)(n + 2)); bput(b, d, n);
}

/* The block value of component c at block (bx, by): gray steps, chroma
 * that varies, or 128 (no chroma) when flat_chroma. */
static int jpeg_val(int c, int bx, int by, int flat_chroma) {
    if (c == 0) return 16 + (bx * 37 + by * 53) % 224;
    if (flat_chroma) return 128;
    return c == 1 ? 64 + (bx * 29 + by * 7) % 128 : 64 + (by * 41 + bx * 3) % 128;
}

/* nc 1 or 3 (4 writes a CMYK frame header for a refusal); sub 2: 4:2:0;
 * marker: the SOF (0xC0 baseline, 0xC2 progressive with one DC scan);
 * prec: the sample precision; app: segments after SOI. */
static void make_jpeg(bb* out, int w, int h, int nc, int sub, int marker, int prec, int flat_chroma, const bb* app) {
    static const uint8_t ac_one[17] = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0x00 };   /* EOB is "0" */
    bb b;
    bitw bw;
    uint8_t t[64 + 32];
    int c, k, prog = marker == 0xC2, mx, my, pred[4] = { 0, 0, 0, 0 };
    memset(&b, 0, sizeof b);
    b8(&b, 0xFF); b8(&b, 0xD8);
    if (app) bput(&b, app->d, app->n);
    t[0] = 0;
    memset(t + 1, 1, 64);
    seg(&b, 0xDB, t, 65);
    t[0] = (uint8_t)prec; t[1] = (uint8_t)(h >> 8); t[2] = (uint8_t)h; t[3] = (uint8_t)(w >> 8); t[4] = (uint8_t)w; t[5] = (uint8_t)nc;
    for (c = 0; c < nc; c++) { t[6 + 3 * c] = (uint8_t)(c + 1); t[7 + 3 * c] = (uint8_t)(c == 0 && sub == 2 ? 0x22 : 0x11); t[8 + 3 * c] = 0; }
    seg(&b, marker, t, 6 + 3 * (size_t)nc);
    t[0] = 0x00;
    memcpy(t + 1, dc_bits, 16);
    for (k = 0; k < 12; k++) t[17 + k] = (uint8_t)k;
    seg(&b, 0xC4, t, 29);
    if (!prog) { t[0] = 0x10; memcpy(t + 1, ac_one, 17); seg(&b, 0xC4, t, 18); }
    t[0] = (uint8_t)nc;
    for (c = 0; c < nc; c++) { t[1 + 2 * c] = (uint8_t)(c + 1); t[2 + 2 * c] = 0x00; }
    t[1 + 2 * nc] = 0; t[2 + 2 * nc] = prog ? 0 : 63; t[3 + 2 * nc] = 0;
    seg(&b, 0xDA, t, 4 + 2 * (size_t)nc);
    memset(&bw, 0, sizeof bw);
    bw.b = &b;
#define BLOCK(c_, bx_, by_) do { int v_ = (jpeg_val((c_), (bx_), (by_), flat_chroma) - 128) * 8; put_dc(&bw, v_ - pred[c_]); pred[c_] = v_; \
                                 if (!prog) bw_put(&bw, 0, 1); } while (0)
    if (nc == 1) {
        for (my = 0; my < (h + 7) / 8; my++)
            for (mx = 0; mx < (w + 7) / 8; mx++) BLOCK(0, mx, my);
    } else if (nc == 3) {
        int m = sub == 2 ? 16 : 8;
        for (my = 0; my < (h + m - 1) / m; my++)
            for (mx = 0; mx < (w + m - 1) / m; mx++) {
                if (sub == 2) { BLOCK(0, 2 * mx, 2 * my); BLOCK(0, 2 * mx + 1, 2 * my); BLOCK(0, 2 * mx, 2 * my + 1); BLOCK(0, 2 * mx + 1, 2 * my + 1); }
                else BLOCK(0, mx, my);
                BLOCK(1, mx, my);
                BLOCK(2, mx, my);
            }
    }
#undef BLOCK
    bw_flush(&bw);
    b8(&b, 0xFF); b8(&b, 0xD9);
    *out = b;
}

/* APP1 Exif with IFD0 orientation o (big-endian TIFF) */
static void app_exif(bb* b, int o) {
    static const uint8_t tiff[] = { 'E', 'x', 'i', 'f', 0, 0, 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 1, 0x01, 0x12, 0, 3, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0 };
    uint8_t t[sizeof tiff];
    memcpy(t, tiff, sizeof t);
    t[24] = 0;
    t[25] = (uint8_t)o;
    seg(b, 0xE1, t, sizeof t);
}

/* APP2 ICC_PROFILE chunks: the profile split in `parts` pieces */
static void app_icc(bb* b, int srgb, int parts) {
    bb p;
    int k;
    size_t at = 0;
    memset(&p, 0, sizeof p);
    make_icc(&p, srgb);
    for (k = 1; k <= parts; k++) {
        size_t n = k == parts ? p.n - at : p.n / (size_t)parts;
        bb s;
        memset(&s, 0, sizeof s);
        bput(&s, "ICC_PROFILE", 12);
        b8(&s, (uint32_t)k); b8(&s, (uint32_t)parts);
        bput(&s, p.d + at, n);
        seg(b, 0xE2, s.d, s.n);
        at += n;
        free(s.d);
    }
    free(p.d);
}

/* --- .xlsx workbooks: a zip of XML parts, some stored and some deflated
 * with the fixed Huffman code (written here; nothing to link) ------------- */

static uint32_t bit_rev(uint32_t v, int n) {
    uint32_t r = 0;
    int i;
    for (i = 0; i < n; i++) r |= ((v >> i) & 1u) << (n - 1 - i);
    return r;
}

static void deflate_fixed(bb* out, const uint8_t* d, size_t n) {
    uint32_t acc = 0;
    int bits = 0;
    size_t i;
#define PUT(v, l) do { acc |= (uint32_t)(v) << bits; bits += (l); while (bits >= 8) { b8(out, acc & 255); acc >>= 8; bits -= 8; } } while (0)
    PUT(1, 1);   /* the last block */
    PUT(1, 2);   /* fixed Huffman codes */
    for (i = 0; i < n; i++) {
        if (d[i] < 144) PUT(bit_rev(0x30u + d[i], 8), 8);
        else PUT(bit_rev(0x190u + (d[i] - 144u), 9), 9);
    }
    PUT(0, 7);   /* end of block */
    if (bits) b8(out, acc & 255);
#undef PUT
}

typedef struct zw { bb b, cd; int n; } zw;

static void zip_add(zw* z, const char* name, const char* text, int deflate) {
    size_t n = strlen(text), nl = strlen(name);
    uint32_t crc = ypak_crc32(0, text, n), lho = (uint32_t)z->b.n;
    bb data;
    memset(&data, 0, sizeof data);
    if (deflate) deflate_fixed(&data, (const uint8_t*)text, n);
    else bput(&data, text, n);
    le32(&z->b, 0x04034B50u); le16(&z->b, 20); le16(&z->b, 0); le16(&z->b, deflate ? 8 : 0); le16(&z->b, 0); le16(&z->b, 0x21);
    le32(&z->b, crc); le32(&z->b, (uint32_t)data.n); le32(&z->b, (uint32_t)n); le16(&z->b, (uint32_t)nl); le16(&z->b, 0);
    bput(&z->b, name, nl);
    bput(&z->b, data.d, data.n);
    le32(&z->cd, 0x02014B50u); le16(&z->cd, 20); le16(&z->cd, 20); le16(&z->cd, 0); le16(&z->cd, deflate ? 8 : 0); le16(&z->cd, 0);
    le16(&z->cd, 0x21); le32(&z->cd, crc); le32(&z->cd, (uint32_t)data.n); le32(&z->cd, (uint32_t)n); le16(&z->cd, (uint32_t)nl);
    le16(&z->cd, 0); le16(&z->cd, 0); le16(&z->cd, 0); le16(&z->cd, 0); le32(&z->cd, 0); le32(&z->cd, lho);
    bput(&z->cd, name, nl);
    z->n++;
    free(data.d);
}

#define XL_NS "xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" " \
              "xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
#define XL_REL "http://schemas.openxmlformats.org/officeDocument/2006/relationships/"

/* A workbook with the sheets "Conditions" (sheet1, given) and "Sheet2". */
static void make_xlsx(bb* out, const char* sheet1) {
    zw z;
    static const char* const strings =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<sst " XL_NS " count=\"9\" uniqueCount=\"9\">"
        "<si><t>word</t></si><si><t>contrast</t></si><si><t>code</t></si><si><t>flag</t></si><si><t>cat</t></si>"
        "<si><t>007</t></si><si><r><t>d&lt;o</t></r><r><rPr><b/></rPr><t>g</t></r><rPh sb=\"0\" eb=\"1\"><t>DOG</t></rPh></si>"
        "<si><t>x_x0041_y</t></si><si><t>&#x3b1;&#946;</t></si></sst>";
    static const char* const styles =
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n"
        "<styleSheet " XL_NS "><numFmts count=\"1\"><numFmt numFmtId=\"164\" formatCode=\"yyyy\\-mm\\-dd\"/></numFmts>"
        "<cellXfs count=\"4\"><xf numFmtId=\"0\"/><xf numFmtId=\"164\" applyNumberFormat=\"1\"/><xf numFmtId=\"14\"/><xf numFmtId=\"2\"/></cellXfs></styleSheet>";
    memset(&z, 0, sizeof z);
    zip_add(&z, "[Content_Types].xml", "<?xml version=\"1.0\"?><Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\"/>", 0);
    zip_add(&z, "_rels/.rels", "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                               "<Relationship Id=\"rId1\" Type=\"" XL_REL "officeDocument\" Target=\"xl/workbook.xml\"/></Relationships>", 0);
    zip_add(&z, "xl/workbook.xml", "<?xml version=\"1.0\"?><workbook " XL_NS "><sheets><sheet name=\"Conditions\" sheetId=\"1\" r:id=\"rId1\"/>"
                                   "<sheet name=\"Sheet2\" sheetId=\"2\" r:id=\"rId2\"/></sheets></workbook>", 1);
    zip_add(&z, "xl/_rels/workbook.xml.rels", "<?xml version=\"1.0\"?><Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
                                              "<Relationship Id=\"rId1\" Type=\"" XL_REL "worksheet\" Target=\"worksheets/sheet1.xml\"/>"
                                              "<Relationship Id=\"rId2\" Type=\"" XL_REL "worksheet\" Target=\"/xl/worksheets/sheet2.xml\"/>"
                                              "<Relationship Id=\"rId3\" Type=\"" XL_REL "sharedStrings\" Target=\"sharedStrings.xml\"/>"
                                              "<Relationship Id=\"rId4\" Type=\"" XL_REL "styles\" Target=\"./styles.xml\"/></Relationships>", 0);
    zip_add(&z, "xl/sharedStrings.xml", strings, 1);
    zip_add(&z, "xl/styles.xml", styles, 0);
    zip_add(&z, "xl/worksheets/sheet1.xml", sheet1, 1);
    zip_add(&z, "xl/worksheets/sheet2.xml", "<worksheet " XL_NS "><sheetData><row r=\"1\"><c r=\"A1\" t=\"inlineStr\"><is><t>n</t></is></c></row>"
                                            "<row r=\"2\"><c r=\"A2\"><v>3</v></c></row></sheetData></worksheet>", 1);
    {
        uint32_t cdo = (uint32_t)z.b.n;
        bput(&z.b, z.cd.d, z.cd.n);
        le32(&z.b, 0x06054B50u); le16(&z.b, 0); le16(&z.b, 0); le16(&z.b, (uint32_t)z.n); le16(&z.b, (uint32_t)z.n);
        le32(&z.b, (uint32_t)z.cd.n); le32(&z.b, cdo); le16(&z.b, 0);
    }
    *out = z.b;
    free(z.cd.d);
}

/* The conditions sheet: shared, inline and formula strings, a boolean, a
 * cached formula, an empty row, cells without references (row 5). */
#define XL_SHEET_HEAD "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n<worksheet " XL_NS "><dimension ref=\"A1:F5\"/><sheetData>" \
    "<row r=\"1\"><c r=\"A1\" t=\"s\"><v>0</v></c><c r=\"B1\" t=\"s\"><v>1</v></c><c r=\"C1\" t=\"s\"><v>2</v></c><c r=\"D1\" t=\"s\"><v>3</v></c>" \
    "<c r=\"E1\" t=\"inlineStr\"><is><t>note</t></is></c><c r=\"F1\" s=\"3\"/></row>"
static const char* const g_sheet1 = XL_SHEET_HEAD
    "<row r=\"2\"><c r=\"A2\" t=\"s\"><v>4</v></c><c r=\"B2\"><v>0.25</v></c><c r=\"C2\" t=\"s\"><v>5</v></c><c r=\"D2\" t=\"b\"><v>1</v></c>"
    "<c r=\"E2\" t=\"inlineStr\"><is><r><t>a, </t></r><r><rPr/><t xml:space=\"preserve\">\"b\"</t></r></is></c></row>"
    "<row r=\"3\"><c r=\"A3\" t=\"s\"><v>6</v></c><c r=\"B3\" s=\"3\"><f>B2*4</f><v>1</v></c><c r=\"C3\" t=\"s\"><v>7</v></c><c r=\"D3\" t=\"b\"><v>0</v></c></row>"
    "<row r=\"4\" spans=\"1:6\"><c r=\"B4\" s=\"3\"/></row>"
    "<row r=\"5\"><c t=\"s\"><v>8</v></c><c><v>1.0000000000000001E-2</v></c><c t=\"s\"><v>5</v></c><c t=\"b\"><v>0</v></c>"
    "<c t=\"str\"><f>\"x\"&amp;\"y\"</f><v>xy</v></c></row></sheetData></worksheet>";

#endif
